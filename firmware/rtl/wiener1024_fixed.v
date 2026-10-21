`timescale 1ns/1ps
// Candidate v2: original fixed-parameter Wiener, FFT1024/hop512.
// 3 float32 beams -> Q30, sqrt periodic Hann,3 forward/unscaled FFTs,
// fixed power-ratio gain, inverse unscaled FFT with explicit /1024,
// weighted overlap-add and source-equivalent clamp, then float32.
// Coefficients Q16 are frozen for a stream. Changing them requires rst_n.
// For single-point pass selected single coefficient as c1 and c2=0;
// for two-point pass the independently selected two coefficients.
// Continuous input must average <=48kHz at100MHz. Output may burst; the
// 1024-element output FIFO decouples steady48kHz transport. A deadline or
// FIFO overrun latches fault, never silently substitutes software output.
module wiener1024_fixed #(
    parameter WINDOW_FILE="model_results/sqrt_hann_q30.mem"
) (
    input wire clk,input wire rst_n,
    input wire in_valid,output wire in_ready,
    input wire [31:0] in_a,input wire [31:0] in_b1,input wire [31:0] in_b2,
    input wire [31:0] in_sample_index,
    input wire [23:0] coefficient1_q16,input wire [23:0] coefficient2_q16,
    output wire out_valid,input wire out_ready,output wire [31:0] out_sample,
    output wire [31:0] out_sample_index,
    output reg fault,output reg [31:0] error_count,
    output reg [31:0] frames_completed
);
    localparam IDLE=0,CONFIG=1,READ=2,MULT_WINDOW=3,QUANT_INPUT=4,SEND=5,
      CAPTURE=6,POWER_QUANT=7,POWER_MUL=8,POWER_SUM=9,POWER_WEIGHT=10,
      POWER_STORE=11,GAIN_INIT=12,GAIN_DIV=13,GAIN_MUL=14,GAIN_STORE=15,
      IFFT_SHIFT=16,IFFT_INPUT=17,SYNTH_MUL=18,SYNTH_ROUND=19,SYNTH_STORE=20,
      SYNTH_CLIP=21,SYNTH_FLOAT=22,SYNTH_PUSH=23,
      FAILED=31;
    localparam signed [31:0] ONE=32'sd1073741824;
    reg [4:0] state;
    reg [1:0] pass;
    reg [9:0] input_index,output_index,bin_index;
    reg [10:0] write_pointer,frame_base;
    reg [10:0] fill_count;
    reg [8:0] hop_count;
    reg [31:0] received_count;
    reg parameter_valid;
    reg [23:0] c1,c2;
    (* ram_style="distributed" *) reg [95:0] input_ring[0:2047];
    reg [95:0] ring_data;
    (* ram_style="distributed" *) reg [95:0] noise_power[0:1023];
    reg [95:0] noise_data;
    (* ram_style="distributed" *) reg [85:0] reduced_spectrum[0:1023];
    reg [85:0] spectrum_data;
    (* rom_style="distributed" *) reg [31:0] window[0:1023];
    (* ram_style="distributed" *) reg signed [47:0] overlap[0:511];
    reg overlap_valid;
    (* ram_style="distributed" *) reg [31:0] output_fifo[0:1023];
    reg [9:0] fifo_write,fifo_read;
    reg [10:0] fifo_count;
    reg [31:0] popped_count;
    initial $readmemh(WINDOW_FILE,window);
    function signed [31:0] float_to_q30;
        input [31:0] bits;
        integer shift;
        reg [63:0] magnitude,tail,half;
        begin
            magnitude=0;tail=0;half=0;shift=0;
            if(bits[30:23]>=127) magnitude=64'd1073741824;
            else if(bits[30:23]>=95) begin
                magnitude={40'd0,1'b1,bits[22:0]};
                if(bits[30:23]>=120) magnitude=magnitude<<(bits[30:23]-120);
                else begin
                    shift=120-bits[30:23];
                    tail=magnitude&((64'd1<<shift)-1);
                    half=64'd1<<(shift-1);
                    magnitude=magnitude>>shift;
                    if(tail>half || (tail==half && magnitude[0])) magnitude=magnitude+1'b1;
                end
            end
            float_to_q30=bits[31] ? -$signed(magnitude[31:0]) : $signed(magnitude[31:0]);
        end
    endfunction
    function signed [31:0] round_q60;
        input signed [63:0] value;
        reg [63:0] magnitude;
        begin
            magnitude=value[63] ? -value:value;
            magnitude=(magnitude+64'd536870912)>>30;
            round_q60=value[63] ? -$signed(magnitude[31:0]):$signed(magnitude[31:0]);
        end
    endfunction
    function signed [39:0] power_component;
        input signed [42:0] value;
        reg [42:0] magnitude;
        begin
            magnitude=value[42] ? -value:value;
            magnitude=(magnitude+43'd8)>>4;
            power_component=value[42] ? -$signed(magnitude[39:0]):$signed(magnitude[39:0]);
        end
    endfunction
    function signed [47:0] round_product;
        input signed [74:0] value;
        input [5:0] shift;
        reg [74:0] magnitude;
        begin
            magnitude=value[74] ? -value:value;
            if(shift!=0) magnitude=(magnitude+(75'd1<<(shift-1)))>>shift;
            round_product=value[74] ? -$signed(magnitude[47:0]):$signed(magnitude[47:0]);
        end
    endfunction
    function signed [31:0] shift_spectrum;
        input signed [42:0] value;
        input [5:0] shift;
        reg [42:0] magnitude;
        begin
            magnitude=value[42] ? -value:value;
            if(shift!=0) magnitude=(magnitude+(43'd1<<(shift-1)))>>shift;
            shift_spectrum=value[42] ? -$signed(magnitude[31:0]):$signed(magnitude[31:0]);
        end
    endfunction
    function [31:0] q30_to_float;
        input signed [31:0] value;
        reg [31:0] magnitude;
        reg [24:0] mantissa;
        reg [31:0] tail,half;
        reg [7:0] exponent;
        integer p,j,shift;
        begin
            magnitude=value[31] ? -value:value;
            p=0;mantissa=0;tail=0;half=0;shift=0;exponent=0;
            for(j=0;j<32;j=j+1)if(magnitude[j])p=j;
            if(magnitude==0)q30_to_float=0;
            else begin
                exponent=p+97;
                if(p<=23)mantissa=magnitude<<(23-p);
                else begin
                    shift=p-23;mantissa=magnitude>>shift;
                    tail=magnitude&((32'd1<<shift)-1);half=32'd1<<(shift-1);
                    if(tail>half || (tail==half && mantissa[0]))mantissa=mantissa+1'b1;
                    if(mantissa[24])begin mantissa=mantissa>>1;exponent=exponent+1'b1;end
                end
                q30_to_float={value[31],exponent,mantissa[22:0]};
            end
        end
    endfunction

    wire take_input=in_valid && in_ready;
    wire frame_trigger=take_input && (fill_count==1023 || (fill_count==1024 && hop_count==511));
    wire input_nonfinite=in_a[30:23]==255 || in_b1[30:23]==255 || in_b2[30:23]==255;
    wire input_error=take_input && (in_sample_index!=received_count || input_nonfinite ||
        coefficient1_q16>24'd6553600 || coefficient2_q16>24'd6553600 ||
        (parameter_valid && (coefficient1_q16!=c1 || coefficient2_q16!=c2)));
    wire frame_overrun=frame_trigger && state!=IDLE;
    assign in_ready=!fault;
    wire [10:0] ring_address=frame_base+{1'b0,input_index};
    always @(posedge clk) begin
        if(take_input)input_ring[write_pointer]<={float_to_q30(in_b2),float_to_q30(in_b1),float_to_q30(in_a)};
        if(state==READ)begin ring_data<=input_ring[ring_address];spectrum_data<=reduced_spectrum[input_index];end
        if(state==POWER_QUANT)noise_data<=noise_power[bin_index];
    end
    always @(posedge clk)begin
        if(!rst_n)begin
            write_pointer<=0;fill_count<=0;hop_count<=0;received_count<=0;
            c1<=0;c2<=0;parameter_valid<=0;
        end else if(take_input)begin
            write_pointer<=write_pointer+1'b1;received_count<=received_count+1'b1;
            if(!parameter_valid)begin c1<=coefficient1_q16;c2<=coefficient2_q16;parameter_valid<=1;end
            if(fill_count<1024)begin fill_count<=fill_count+1'b1;hop_count<=0;end
            else hop_count<=hop_count+1'b1;
        end
    end
    reg [63:0] fft_input;
    wire fft_config_ready,fft_input_ready,fft_output_valid,fft_output_last;
    wire [95:0] fft_output;
    wire [15:0] fft_user;
    wire event_started,event_unexpected,event_missing,event_status_halt,event_in_halt,event_out_halt;
    denoise_fft1024 u_fft(
        .aclk(clk),.aresetn(rst_n),
        .s_axis_config_tdata(pass==3 ? 8'd0:8'd1),.s_axis_config_tvalid(state==CONFIG),.s_axis_config_tready(fft_config_ready),
        .s_axis_data_tdata(fft_input),.s_axis_data_tvalid(state==SEND),.s_axis_data_tready(fft_input_ready),.s_axis_data_tlast(input_index==1023),
        .m_axis_data_tdata(fft_output),.m_axis_data_tuser(fft_user),.m_axis_data_tvalid(fft_output_valid),.m_axis_data_tready(state==CAPTURE),.m_axis_data_tlast(fft_output_last),
        .event_frame_started(event_started),.event_tlast_unexpected(event_unexpected),.event_tlast_missing(event_missing),
        .event_status_channel_halt(event_status_halt),.event_data_in_channel_halt(event_in_halt),.event_data_out_channel_halt(event_out_halt));
    reg signed [63:0] window_product;
    wire signed [31:0] selected_input=pass==0 ? $signed(ring_data[63:32]) : pass==1 ? $signed(ring_data[95:64]) : $signed(ring_data[31:0]);
    reg signed [42:0] raw_real,raw_imag;
    reg signed [39:0] component_real,component_imag;
    reg [79:0] real_square,imag_square;
    reg [80:0] power_sum;
    reg [104:0] weighted_power;
    wire [105:0] total_noise={1'b0,weighted_power}+(pass==1 ? {10'd0,noise_data}:106'd0);
    reg [96:0] numerator,denominator;
    reg [96:0] division_remainder;
    reg [30:0] gain;
    reg [4:0] division_step;
    wire [97:0] division_trial={division_remainder,1'b0};
    wire division_take=division_trial>={1'b0,denominator};
    wire [96:0] division_next=division_take ? division_trial-{1'b0,denominator} : division_trial;
    wire [30:0] gain_next={1'b0,gain[28:0],division_take};
    reg signed [74:0] gain_real_product,gain_imag_product,synthesis_product;
    wire signed [47:0] rounded_real=round_product(gain_real_product,6'd30);
    wire signed [47:0] rounded_imag=round_product(gain_imag_product,6'd30);
    wire [42:0] magnitude_real=rounded_real[47] ? -rounded_real[42:0] : rounded_real[42:0];
    wire [42:0] magnitude_imag=rounded_imag[47] ? -rounded_imag[42:0] : rounded_imag[42:0];
    reg [42:0] maximum_spectrum;
    reg [5:0] inverse_shift;
    reg signed [47:0] synthesized_sample;
    reg signed [48:0] synthesis_sum;
    reg signed [31:0] output_clipped;
    reg [31:0] output_float;
    wire signed [48:0] overlap_sum=$signed(synthesized_sample)+(overlap_valid ? $signed(overlap[bin_index[8:0]]) : 49'sd0);
    wire signed [31:0] clipped_sample=synthesis_sum>49'sd1073741824 ? ONE : synthesis_sum< -49'sd1073741824 ? -ONE : synthesis_sum[31:0];
    wire fifo_pop=out_valid && out_ready;
    wire fifo_push=state==SYNTH_PUSH && !fault;
    wire fifo_overrun=fifo_push && fifo_count==1024 && !fifo_pop;
    assign out_valid=fifo_count!=0 && !fault;
    assign out_sample=output_fifo[fifo_read];
    assign out_sample_index=popped_count;
    reg numeric_error;
    integer leading,bit_index;
    always @(posedge clk)begin
        if(!rst_n)begin fifo_write<=0;fifo_read<=0;fifo_count<=0;popped_count<=0;end
        else begin
            if(fifo_push && !fifo_overrun)begin output_fifo[fifo_write]<=output_float;fifo_write<=fifo_write+1'b1;end
            if(fifo_pop)begin fifo_read<=fifo_read+1'b1;popped_count<=popped_count+1'b1;end
            case({fifo_push && !fifo_overrun,fifo_pop})
            2'b10:fifo_count<=fifo_count+1'b1;
            2'b01:fifo_count<=fifo_count-1'b1;
            default:;
            endcase
        end
    end
    always @(posedge clk)begin
        if(!rst_n)begin fault<=0;error_count<=0;end
        else if(input_error || frame_overrun || fifo_overrun || numeric_error || event_unexpected || event_missing)begin
            fault<=1;error_count<=error_count+1'b1;
        end
    end
    always @(posedge clk)begin
        if(!rst_n)begin
            state<=IDLE;pass<=0;input_index<=0;output_index<=0;bin_index<=0;frame_base<=0;
            fft_input<=0;window_product<=0;raw_real<=0;raw_imag<=0;component_real<=0;component_imag<=0;
            real_square<=0;imag_square<=0;power_sum<=0;weighted_power<=0;numerator<=0;denominator<=0;
            division_remainder<=0;gain<=0;division_step<=0;gain_real_product<=0;gain_imag_product<=0;
            maximum_spectrum<=0;inverse_shift<=0;synthesis_product<=0;synthesized_sample<=0;
            synthesis_sum<=0;output_clipped<=0;output_float<=0;
            overlap_valid<=0;numeric_error<=0;frames_completed<=0;
        end else begin
            numeric_error<=0;
            if(fault)state<=FAILED;
            else case(state)
            IDLE:if(frame_trigger)begin
                frame_base<=write_pointer+11'd1-11'd1024;
                pass<=0;input_index<=0;output_index<=0;maximum_spectrum<=0;state<=CONFIG;
            end
            CONFIG:if(fft_config_ready)begin input_index<=0;output_index<=0;state<=READ;end
            READ:state<=pass==3 ? IFFT_INPUT:MULT_WINDOW;
            MULT_WINDOW:begin window_product<=selected_input*$signed(window[input_index]);state<=QUANT_INPUT;end
            QUANT_INPUT:begin fft_input<={32'd0,round_q60(window_product)};state<=SEND;end
            IFFT_INPUT:begin fft_input<={shift_spectrum($signed(spectrum_data[85:43]),inverse_shift),shift_spectrum($signed(spectrum_data[42:0]),inverse_shift)};state<=SEND;end
            SEND:if(fft_input_ready)begin
                if(input_index==1023)state<=CAPTURE;
                else begin input_index<=input_index+1'b1;state<=READ;end
            end
            CAPTURE:if(fft_output_valid)begin
                raw_real<=fft_output[42:0];raw_imag<=fft_output[90:48];bin_index<=output_index;
                if(fft_user[9:0]!=output_index || fft_output_last!=(output_index==1023))numeric_error<=1;
                state<=pass==3 ? SYNTH_MUL:POWER_QUANT;
            end
            POWER_QUANT:begin component_real<=power_component(raw_real);component_imag<=power_component(raw_imag);state<=POWER_MUL;end
            POWER_MUL:begin real_square<=component_real*component_real;imag_square<=component_imag*component_imag;state<=POWER_SUM;end
            POWER_SUM:begin power_sum<={1'b0,real_square}+{1'b0,imag_square};state<=POWER_WEIGHT;end
            POWER_WEIGHT:begin
                if(pass==2)begin
                    numerator<={power_sum,16'd0};denominator<={power_sum,16'd0}+{1'b0,noise_data};state<=GAIN_INIT;
                end else begin weighted_power<=power_sum*(pass==0 ? c1:c2);state<=POWER_STORE;end
            end
            POWER_STORE:begin
                if(|total_noise[105:96])numeric_error<=1;
                noise_power[bin_index]<=total_noise[95:0];
                if(output_index==1023)begin pass<=pass+1'b1;input_index<=0;output_index<=0;state<=CONFIG;end
                else begin output_index<=output_index+1'b1;state<=CAPTURE;end
            end
            GAIN_INIT:begin
                if(numerator[96] || denominator[96])numeric_error<=1;
                if(denominator<=97'd65536)begin gain<=0;state<=GAIN_MUL;end
                else if(numerator>=denominator)begin gain<=31'd1073741824;state<=GAIN_MUL;end
                else begin division_remainder<=numerator;gain<=0;division_step<=0;state<=GAIN_DIV;end
            end
            GAIN_DIV:begin
                division_remainder<=division_next;gain<=gain_next;
                if(division_step==29)state<=GAIN_MUL;
                else division_step<=division_step+1'b1;
            end
            GAIN_MUL:begin
                gain_real_product<=raw_real*$signed({1'b0,gain});gain_imag_product<=raw_imag*$signed({1'b0,gain});state<=GAIN_STORE;
            end
            GAIN_STORE:begin
                reduced_spectrum[bin_index]<={rounded_imag[42:0],rounded_real[42:0]};
                if(magnitude_real>maximum_spectrum && magnitude_real>=magnitude_imag)maximum_spectrum<=magnitude_real;
                else if(magnitude_imag>maximum_spectrum)maximum_spectrum<=magnitude_imag;
                if(output_index==1023)state<=IFFT_SHIFT;
                else begin output_index<=output_index+1'b1;state<=CAPTURE;end
            end
            IFFT_SHIFT:begin
                leading=0;
                for(bit_index=0;bit_index<43;bit_index=bit_index+1)if(maximum_spectrum[bit_index])leading=bit_index;
                inverse_shift<=leading>29 ? leading-29:0;
                pass<=3;input_index<=0;output_index<=0;state<=CONFIG;
            end
            SYNTH_MUL:begin synthesis_product<=raw_real*$signed(window[bin_index]);state<=SYNTH_ROUND;end
            SYNTH_ROUND:begin synthesized_sample<=round_product(synthesis_product,6'd40-inverse_shift);state<=SYNTH_STORE;end
            SYNTH_STORE:begin
                if(bin_index>=512)begin
                    overlap[bin_index[8:0]]<=synthesized_sample;
                    if(output_index==1023)begin overlap_valid<=1;frames_completed<=frames_completed+1'b1;state<=IDLE;end
                    else begin output_index<=output_index+1'b1;state<=CAPTURE;end
                end else begin synthesis_sum<=overlap_sum;state<=SYNTH_CLIP;end
            end
            SYNTH_CLIP:begin output_clipped<=clipped_sample;state<=SYNTH_FLOAT;end
            SYNTH_FLOAT:begin output_float<=q30_to_float(output_clipped);state<=SYNTH_PUSH;end
            SYNTH_PUSH:begin output_index<=output_index+1'b1;state<=CAPTURE;end
            default:state<=FAILED;
            endcase
        end
    end
endmodule
