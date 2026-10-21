`timescale 1ns/1ps
// GitHub-equivalent three-beam cubic Lagrange front end, fixed model v1.
// 48 channels share ONE 67x48 signed16 history. Coefficients are Q30.
// Input must present ordered CH0..47 groups, >=1600 clocks between CH0s at
// 100MHz (48kHz source provides ~2083). ready is configuration status,
// not a per-word backpressure signal. Errors/dropped groups are counted.
module denoise_beam3 (
    input wire clk, input wire rst_n,
    input wire [31:0] sample_word, input wire sample_valid,
    input wire [5:0] sample_channel,
    input wire cfg_write, input wire [2:0] cfg_index,
    input wire signed [31:0] cfg_value,
    input wire cfg_commit, input wire [15:0] cfg_id,
    output wire cfg_busy, output reg cfg_applied,
    output reg [15:0] cfg_active_id,
    output reg beam_valid, output reg [31:0] beam_a,
    output reg [31:0] beam_b1, output reg [31:0] beam_b2,
    output reg [31:0] beam_sample_index,
    output reg [15:0] beam_config_id,
    output reg [31:0] overflow_count, output wire ready
);
    localparam C_IDLE=0,C_WAIT=1,C_DELAY=2,C_FRAC=3,C_SQUARE=4,
               C_CUBE=5,C_NUM=6,C_MUL0=7,C_MUL3=8,C_STORE=9,C_APPLY=10,C_SQ_ROUND=11;
    localparam B_IDLE=0,B_ZERO=1,B_COEF_REQ=2,B_COEF_WAIT=3,
               B_COEF_LATCH=4,B_TAPS=5,B_DRAIN=6,B_CONVERT=7,B_FLOAT=8;
    localparam signed [31:0] ONE_Q30=32'sd1073741824;
    localparam signed [31:0] SIXTH_Q30=32'sd178956971;

    reg [3:0] cstate,bstate;
    reg signed [31:0] staged[0:5], preparing[0:5];
    reg [15:0] preparing_id;
    reg active_valid,active_bank,preparing_bank;
    reg [1:0] prepare_beam;
    reg [5:0] prepare_channel;
    reg signed [39:0] delay_q26;
    reg [6:0] prepare_integer;
    reg signed [31:0] fraction_q30,square_q30,cube_q30;
    reg signed [63:0] square_product,cube_product,divide0_product,divide3_product;
    reg signed [31:0] numerator0,numerator2,numerator3;
    // 7-bit integer delay plus four signed Q30 taps. Bank depth=144 each.
    (* ram_style="block" *) reg [134:0] coefficient_memory[0:287];
    reg [8:0] coefficient_address;
    reg [134:0] coefficient_data;
    wire signed [4:0] column_weight=$signed({1'b0,prepare_channel[2:0],1'b0})-5'sd7;
    wire signed [4:0] row_weight=5'sd5-$signed({1'b0,prepare_channel[5:3],1'b0});
    wire signed [36:0] product_x=column_weight*preparing[{prepare_beam,1'b0}];
    wire signed [36:0] product_y=row_weight*preparing[{prepare_beam,1'b0}+1'b1];
    wire signed [39:0] next_delay=40'sd2214592512-{{3{product_x[36]}},product_x}-{{3{product_y[36]}},product_y};
    wire [8:0] prepare_address=(preparing_bank ? 9'd144 : 9'd0)+prepare_beam*9'd48+prepare_channel;
    function signed [31:0] q30_round;
        input signed [63:0] value_in;
        reg [63:0] magnitude;
        begin
            magnitude=value_in[63] ? -value_in : value_in;
            magnitude=(magnitude+64'd536870912)>>30;
            q30_round=value_in[63] ? -$signed(magnitude[31:0]) : $signed(magnitude[31:0]);
        end
    endfunction
    wire signed [31:0] coefficient0=q30_round(divide0_product);
    wire signed [31:0] coefficient2=(numerator2+32'sd1)>>>1;
    wire signed [31:0] coefficient3=q30_round(divide3_product);
    wire signed [31:0] coefficient1=ONE_Q30-coefficient0-coefficient2-coefficient3;
    reg config_error;
    assign cfg_busy=cstate!=C_IDLE;
    assign ready=active_valid;

    // Data before initial applied CH0 is ignored. Initial group index is0,
    // initial66 groups are zero; later direction-only commits retain history.
    reg [5:0] expected_channel;
    reg [6:0] next_slot,group_slot,mature_groups;
    reg [31:0] next_index,group_index;
    reg group_active,group_bank,group_zero;
    reg [15:0] group_config;
    wire input_enabled=active_valid || cstate==C_APPLY;
    wire start_group=sample_valid && input_enabled && sample_channel==0;
    wire ordered_word=sample_valid && input_enabled &&
        (sample_channel==0 || (expected_channel!=0 && sample_channel==expected_channel && sample_channel<48));
    wire end_group=ordered_word && sample_channel==47 && expected_channel==47;
    wire order_error=sample_valid && input_enabled &&
        ((sample_channel==0 && expected_channel!=0) ||
         (sample_channel!=0 && (expected_channel==0 || sample_channel!=expected_channel || sample_channel>=48)));
    wire [6:0] input_slot=sample_channel==0 ? next_slot : group_slot;
    wire [11:0] history_write_address=input_slot*12'd48+sample_channel;
    (* ram_style="block" *) reg signed [15:0] history[0:3215];
    reg signed [15:0] history_data;
    reg [1:0] queue_write,queue_read;
    reg [2:0] queue_count;
    reg [6:0] queue_slot[0:3];
    reg [31:0] queue_index[0:3];
    reg [15:0] queue_config[0:3];
    reg queue_bank[0:3],queue_zero[0:3];
    wire dequeue=bstate==B_IDLE && queue_count!=0;
    wire enqueue=end_group && group_active && (queue_count<4 || dequeue);
    wire queue_overflow=end_group && group_active && !enqueue;
    integer i;
    always @(posedge clk) begin
        if(!rst_n) begin
            cstate<=C_IDLE; cfg_applied<=0; cfg_active_id<=0;
            active_valid<=0; active_bank<=0; preparing_bank<=1;
            prepare_beam<=0;prepare_channel<=0;preparing_id<=0;
            delay_q26<=0;prepare_integer<=0;fraction_q30<=0;square_q30<=0;cube_q30<=0;
            square_product<=0;cube_product<=0;divide0_product<=0;divide3_product<=0;
            numerator0<=0;numerator2<=0;numerator3<=0;config_error<=0;
            for(i=0;i<6;i=i+1) begin staged[i]<=0;preparing[i]<=0;end
        end else begin
            cfg_applied<=0;config_error<=0;
            if(cfg_write) begin
                if(cstate==C_IDLE && cfg_index<6) staged[cfg_index]<=cfg_value;
                else config_error<=1;
            end
            if(cfg_commit && cstate!=C_IDLE) config_error<=1;
            case(cstate)
            C_IDLE: if(cfg_commit) begin
                for(i=0;i<6;i=i+1) preparing[i]<=staged[i];
                preparing_id<=cfg_id; preparing_bank<=!active_bank;
                prepare_beam<=0; prepare_channel<=0; cstate<=C_WAIT;
            end
            // Old bank readers must have drained before reusing the other bank.
            C_WAIT: if(bstate==B_IDLE && queue_count==0 && expected_channel==0) cstate<=C_DELAY;
            C_DELAY: begin delay_q26<=next_delay;cstate<=C_FRAC;end
            C_FRAC: begin
                if(delay_q26<40'sd67108864 || delay_q26>=40'sd4362076160) begin
                    config_error<=1;cstate<=C_IDLE;
                end else begin
                    prepare_integer<=delay_q26[32:26];
                    fraction_q30<=$signed({2'b00,delay_q26[25:0],4'b0000});
                    cstate<=C_SQUARE;
                end
            end
            C_SQUARE: begin square_product<=fraction_q30*fraction_q30;cstate<=C_SQ_ROUND;end
            C_SQ_ROUND: begin square_q30<=q30_round(square_product);cstate<=C_CUBE;end
            C_CUBE: begin cube_product<=square_q30*fraction_q30;cstate<=C_NUM;end
            C_NUM: begin
                cube_q30<=q30_round(cube_product);
                numerator0<=-q30_round(cube_product)+3*square_q30-2*fraction_q30;
                numerator2<=-q30_round(cube_product)+square_q30+2*fraction_q30;
                numerator3<=q30_round(cube_product)-fraction_q30;
                cstate<=C_MUL0;
            end
            C_MUL0: begin divide0_product<=numerator0*SIXTH_Q30;cstate<=C_MUL3;end
            C_MUL3: begin divide3_product<=numerator3*SIXTH_Q30;cstate<=C_STORE;end
            C_STORE: begin
                if(prepare_channel==47) begin
                    prepare_channel<=0;
                    if(prepare_beam==2) cstate<=C_APPLY;
                    else begin prepare_beam<=prepare_beam+1'b1;cstate<=C_DELAY;end
                end else begin prepare_channel<=prepare_channel+1'b1;cstate<=C_DELAY;end
            end
            C_APPLY: if(start_group) begin
                active_valid<=1;active_bank<=preparing_bank;
                cfg_active_id<=preparing_id;cfg_applied<=1;cstate<=C_IDLE;
            end
            default:cstate<=C_IDLE;
            endcase
        end
    end
    always @(posedge clk) begin
        if(cstate==C_STORE) coefficient_memory[prepare_address]<={prepare_integer,coefficient3,coefficient2,coefficient1,coefficient0};
        coefficient_data<=coefficient_memory[coefficient_address];
        if(ordered_word) history[history_write_address]<=sample_word[23:8];
    end
    always @(posedge clk) begin
        if(!rst_n) begin
            expected_channel<=0;next_slot<=0;group_slot<=0;mature_groups<=0;
            next_index<=0;group_index<=0;group_active<=0;group_bank<=0;group_zero<=1;group_config<=0;
            queue_write<=0;queue_read<=0;queue_count<=0;
        end else begin
            if(start_group) begin
                group_slot<=next_slot;next_slot<=next_slot==66 ? 0 : next_slot+1'b1;
                group_index<=next_index;next_index<=next_index+1'b1;
                group_active<=1;group_bank<=cstate==C_APPLY ? preparing_bank : active_bank;
                group_config<=cstate==C_APPLY ? preparing_id : cfg_active_id;
                group_zero<=mature_groups<66 || order_error;
                expected_channel<=1;
                if(order_error) mature_groups<=0;
            end else if(sample_valid && input_enabled) begin
                if(ordered_word) begin
                    if(sample_channel==47) begin
                        expected_channel<=0;group_active<=0;
                        if(mature_groups<66) mature_groups<=mature_groups+1'b1;
                    end else expected_channel<=expected_channel+1'b1;
                end else begin expected_channel<=0;group_active<=0;mature_groups<=0;end
            end
            if(enqueue) begin
                queue_slot[queue_write]<=group_slot;queue_index[queue_write]<=group_index;
                queue_config[queue_write]<=group_config;queue_bank[queue_write]<=group_bank;
                queue_zero[queue_write]<=group_zero;queue_write<=queue_write+1'b1;
            end
            if(dequeue) queue_read<=queue_read+1'b1;
            case({enqueue,dequeue})
            2'b10:queue_count<=queue_count+1'b1;
            2'b01:queue_count<=queue_count-1'b1;
            default:;
            endcase
        end
    end

    reg [6:0] work_slot;
    reg [31:0] work_index;
    reg [15:0] work_config;
    reg work_bank;
    reg [1:0] work_beam,tap_index;
    reg [5:0] work_channel;
    reg [6:0] integer_delay;
    reg [127:0] taps;
    reg signed [55:0] accumulator;
    reg [2:0] drain;
    wire signed [8:0] lag=$signed({2'b00,integer_delay})+$signed({7'b0,tap_index})-9'sd1;
    wire signed [8:0] slot_difference=$signed({2'b0,work_slot})-lag;
    wire [6:0] read_slot=slot_difference[8] ? slot_difference+9'd67 : slot_difference;
    wire [11:0] history_read_address=read_slot*12'd48+work_channel;
    wire history_read_enable=bstate==B_TAPS;
    reg read_valid,product_valid;
    reg signed [31:0] read_coefficient;
    reg signed [47:0] product;
    reg [31:0] result_a,result_b1;
    wire converter_busy,converter_valid;
    wire [31:0] converter_result;
    wire converter_start=bstate==B_CONVERT;
    reg stale_error;
    denoise_fixed_to_float u_float(.clk(clk),.rst_n(rst_n),.start(converter_start),
        .value(accumulator),.busy(converter_busy),.valid(converter_valid),.result(converter_result));
    always @(posedge clk) begin
        if(history_read_enable) history_data<=history[history_read_address];
        if(!rst_n) begin read_valid<=0;product_valid<=0;read_coefficient<=0;product<=0;end
        else begin
            read_valid<=history_read_enable;product_valid<=read_valid;
            if(history_read_enable) begin
                case(tap_index)
                0:read_coefficient<=$signed(taps[31:0]);
                1:read_coefficient<=$signed(taps[63:32]);
                2:read_coefficient<=$signed(taps[95:64]);
                3:read_coefficient<=$signed(taps[127:96]);
                endcase
            end
            if(read_valid) product<=history_data*read_coefficient;
        end
    end
    always @(posedge clk) begin
        if(!rst_n) begin
            bstate<=B_IDLE;beam_valid<=0;beam_a<=0;beam_b1<=0;beam_b2<=0;
            beam_sample_index<=0;beam_config_id<=0;work_slot<=0;work_index<=0;work_config<=0;work_bank<=0;
            work_beam<=0;work_channel<=0;tap_index<=0;integer_delay<=0;taps<=0;
            accumulator<=0;drain<=0;coefficient_address<=0;result_a<=0;result_b1<=0;stale_error<=0;
        end else begin
            beam_valid<=0;stale_error<=0;
            if(product_valid) accumulator<=accumulator+{{8{product[47]}},product};
            case(bstate)
            B_IDLE: if(dequeue) begin
                work_slot<=queue_slot[queue_read];work_index<=queue_index[queue_read];
                work_config<=queue_config[queue_read];work_bank<=queue_bank[queue_read];
                work_beam<=0;work_channel<=0;accumulator<=0;
                bstate<=queue_zero[queue_read] ? B_ZERO : B_COEF_REQ;
            end
            B_ZERO: begin
                beam_a<=0;beam_b1<=0;beam_b2<=0;beam_sample_index<=work_index;
                beam_config_id<=work_config;beam_valid<=1;bstate<=B_IDLE;
            end
            B_COEF_REQ: begin
                coefficient_address<=(work_bank ? 9'd144 : 9'd0)+work_beam*9'd48+work_channel;
                bstate<=B_COEF_WAIT;
            end
            B_COEF_WAIT:bstate<=B_COEF_LATCH;
            B_COEF_LATCH: begin integer_delay<=coefficient_data[134:128];taps<=coefficient_data[127:0];tap_index<=0;bstate<=B_TAPS;end
            B_TAPS: begin
                tap_index<=tap_index+1'b1;
                if(tap_index==3) begin
                    if(work_channel==47) begin drain<=0;bstate<=B_DRAIN;end
                    else begin work_channel<=work_channel+1'b1;bstate<=B_COEF_REQ;end
                end
            end
            B_DRAIN: begin
                if(drain==3) bstate<=B_CONVERT;
                else drain<=drain+1'b1;
            end
            B_CONVERT:bstate<=B_FLOAT;
            B_FLOAT:if(converter_valid) begin
                if(work_beam==0) result_a<=converter_result;
                if(work_beam==1) result_b1<=converter_result;
                if(work_beam==2) begin
                    if((next_index-work_index)<67) begin
                        beam_a<=result_a;beam_b1<=result_b1;beam_b2<=converter_result;
                        beam_sample_index<=work_index;beam_config_id<=work_config;beam_valid<=1;
                    end else stale_error<=1;
                    bstate<=B_IDLE;
                end else begin work_beam<=work_beam+1'b1;work_channel<=0;accumulator<=0;bstate<=B_COEF_REQ;end
            end
            default:bstate<=B_IDLE;
            endcase
        end
    end
    always @(posedge clk) begin
        if(!rst_n) overflow_count<=0;
        else overflow_count<=overflow_count+{31'd0,config_error}+{31'd0,order_error}+{31'd0,queue_overflow}+{31'd0,stale_error};
    end
endmodule
