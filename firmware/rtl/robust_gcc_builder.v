`timescale 1ns/1ps
// Weighted circular phase mean for every physical displacement group and bin.
// Reliability-weighted IFFT, band normalization and time-domain EMA.
module robust_gcc_builder(
 input wire clk,rst_n,start,history_active,input wire [81:0] history_groups,
 output wire [15:0] phase_address_a,phase_address_b,input wire [31:0] phase_value_a,phase_value_b,
 output wire [5:0] quality_channel_a,quality_channel_b,input wire [15:0] quality_a,quality_b,
 input wire [14:0] gcc_read_address,output reg signed [31:0] gcc_read_value,
 output reg [81:0] valid_groups,output reg [6:0] valid_group_count,
 output reg group_quality_valid,output reg [6:0] group_quality_index,output reg [15:0] group_quality_q15,
 output reg busy,done,window_valid,fft_protocol_error
);
 localparam BINS=687,BAND_SUM=22089349;
 localparam IDLE=0,BIN_INIT=1,PAIR_READ=2,PAIR_WAIT=3,PAIR_MULT=4,PAIR_ACC=5,PAIR_NEXT=6,
  AVG_START=7,AVG_WAIT=8,SQRT_START=9,SQRT_WAIT=10,UNIT_START=11,UNIT_WAIT=12,BIN_SAVE=13,BIN_NEXT=14,
  REL_START=15,REL_WAIT=16,SCALE_START=17,SCALE_WAIT=18,FFT_CONFIG=19,FFT_READ=20,FFT_READ_WAIT=21,FFT_SEND=22,FFT_DRAIN=23,GROUP_NEXT=24,FINISH=25,PAIR_LATCH=26;
 reg [5:0] state;
 reg [6:0] group_index;
 reg [9:0] bin_index;
 reg [10:0] pair_index;
 reg [11:0] fft_index;
 (* rom_style="distributed" *)reg [27:0] groups[0:81];
 (* rom_style="block" *)reg [11:0] pairs[0:1127];
 (* rom_style="block" *)reg [15:0] band[0:BINS-1],coherence[0:1024];
 initial begin $readmemh("rom/groups.mem",groups);$readmemh("rom/pairs.mem",pairs);$readmemh("rom/band_q15.mem",band);$readmemh("rom/coherence_q15.mem",coherence);end
 wire [10:0] pair_offset=groups[group_index][27:17];
 wire [5:0] pair_count=groups[group_index][16:11];
 reg [11:0] pair_word;
 assign quality_channel_a=pair_word[11:6];assign quality_channel_b=pair_word[5:0];
 assign phase_address_a=quality_channel_a*BINS+bin_index;
 assign phase_address_b=quality_channel_b*BINS+bin_index;
 reg signed [15:0] ar,ai,br,bi;
 wire signed [31:0] rr=br*ar,ii=bi*ai,ir=bi*ar,ri=br*ai;
 reg signed [32:0] cross_re,cross_im;
 reg [15:0] pair_weight;
 wire [31:0] pair_weight_product=quality_a*quality_b;
 reg signed [63:0] sum_re,sum_im;
 reg [31:0] weight_sum;
 wire signed [50:0] contribution_re=cross_re*$signed({1'b0,pair_weight});
 wire signed [50:0] contribution_im=cross_im*$signed({1'b0,pair_weight});
 reg signed [15:0] mean_re,mean_im;
 wire signed [31:0] mean_r2=mean_re*mean_re,mean_i2=mean_im*mean_im;
 wire [33:0] mean_magnitude_squared={2'd0,mean_r2}+{2'd0,mean_i2};
 wire sqrt_busy,sqrt_done;wire [16:0] magnitude;
 robust_isqrt #(.W(34))u_sqrt(clk,rst_n,state==SQRT_START,mean_magnitude_squared,sqrt_busy,sqrt_done,magnitude);
 wire ratio_start=(state==AVG_START)||(state==UNIT_START);
 wire signed [63:0] ratio_real=(state==AVG_START)?sum_re:{{48{mean_re[15]}},mean_re};
 wire signed [63:0] ratio_imag=(state==AVG_START)?sum_im:{{48{mean_im[15]}},mean_im};
 wire [63:0] ratio_den=(state==AVG_START)?({32'd0,weight_sum}<<15):{47'd0,magnitude};
 wire ratio_busy,ratio_done;wire signed [15:0] normalized_re,normalized_im;
 robust_complex_ratio u_ratio(clk,rst_n,ratio_start,ratio_real,ratio_imag,ratio_den,ratio_busy,ratio_done,normalized_re,normalized_im);
 reg [15:0] bin_weight;
 wire [10:0] coherence_index=magnitude>=32768?11'd1024:((magnitude+16)>>5);
 wire [31:0] bin_weight_product=coherence[coherence_index]*band[bin_index];
 wire signed [33:0] weighted_re=normalized_re*$signed({1'b0,bin_weight});
 wire signed [33:0] weighted_im=normalized_im*$signed({1'b0,bin_weight});
 wire [47:0] phat_write_data=bin_weight==0?48'd0:{weighted_im[30:7],weighted_re[30:7]};
 reg [31:0] reliable_sum;
 wire rel_busy,rel_done;wire [14:0] reliable_fraction;
 robust_fraction_divider u_reliable(clk,rst_n,state==REL_START,{32'd0,reliable_sum},64'd22089349,rel_busy,rel_done,reliable_fraction);
 wire scale_busy,scale_done;wire [63:0] scale_quotient;
 reg [31:0] scale_q16;
 robust_integer_divider u_scale(clk,rst_n,state==SCALE_START,64'd8796093022208,{32'd0,reliable_sum},scale_busy,scale_done,scale_quotient);
 reg [47:0] fft_word;
 wire [47:0] fft_out;wire [15:0] fft_user;
 wire fft_out_valid,fft_out_last,fft_ready,config_ready;
 wire unexpected,missing;reg fft_frame_done;
 wire [11:0] positive_bin=fft_index>=3401?4096-fft_index:fft_index;
 wire retained_input=(positive_bin>=9)&&(positive_bin<=695);
 wire [47:0] spectrum_word;
 wire [9:0] phat_read_address=positive_bin-9;
 robust_sdp_ram #(.WIDTH(48),.DEPTH(BINS))u_spectrum(clk,rst_n&&state==BIN_SAVE,bin_index,phat_write_data,phat_read_address,spectrum_word);
 wire signed [23:0] spectral_re=spectrum_word[23:0],spectral_im=spectrum_word[47:24];
 robust_fft4096 u_fft(
  .aclk(clk),.aresetn(rst_n),.s_axis_config_tdata(16'h1554),.s_axis_config_tvalid(state==FFT_CONFIG),.s_axis_config_tready(config_ready),
  .s_axis_data_tdata(fft_word),.s_axis_data_tvalid(state==FFT_SEND),.s_axis_data_tready(fft_ready),.s_axis_data_tlast(fft_index==4095),
  .m_axis_data_tdata(fft_out),.m_axis_data_tuser(fft_user),.m_axis_data_tvalid(fft_out_valid),.m_axis_data_tready(1'b1),.m_axis_data_tlast(fft_out_last),
  .event_frame_started(),.event_tlast_unexpected(unexpected),.event_tlast_missing(missing),.event_status_channel_halt(),.event_data_in_channel_halt(),.event_data_out_channel_halt());
 (* ram_style="block" *)reg signed [31:0] gcc[0:82*256-1];
 reg pending=0,pending_history;
 reg [14:0] pending_address;
 reg signed [31:0] pending_value;
 wire keep_lag=fft_user[11:0]<128||fft_user[11:0]>=3968;
 wire new_gcc=fft_out_valid&&keep_lag;
 wire [14:0] write_address={group_index,fft_user[7:0]};
 wire signed [23:0] ifft_real=fft_out[23:0];
 wire signed [56:0] scaled_ifft=ifft_real*$signed({1'b0,scale_q16});
 wire signed [31:0] fresh_correlation=scaled_ifft>>>16;
 wire signed [50:0] mixed=gcc_read_value*18'sd21299+pending_value*18'sd11469;
 always @(posedge clk)begin
  gcc_read_value<=gcc[new_gcc?write_address:gcc_read_address];
  pending<=new_gcc;
  if(new_gcc)begin pending_address<=write_address;pending_value<=fresh_correlation;pending_history<=history_active&&history_groups[group_index];end
  if(pending)gcc[pending_address]<=pending_history?(mixed>>>15):pending_value;
 end
 always @(posedge clk)begin
  if(!rst_n)begin state<=IDLE;group_index<=0;bin_index<=0;pair_index<=0;fft_index<=0;valid_groups<=0;valid_group_count<=0;busy<=0;done<=0;window_valid<=0;fft_protocol_error<=0;group_quality_valid<=0;reliable_sum<=0;fft_frame_done<=0;end
  else begin
   done<=0;group_quality_valid<=0;
   if(unexpected||missing)fft_protocol_error<=1;
   if(fft_out_valid&&fft_out_last)fft_frame_done<=1;
   case(state)
    IDLE:if(start)begin busy<=1;window_valid<=0;valid_groups<=0;valid_group_count<=0;group_index<=0;bin_index<=0;reliable_sum<=0;state<=BIN_INIT;end
    BIN_INIT:begin sum_re<=0;sum_im<=0;weight_sum<=0;pair_index<=pair_offset;state<=PAIR_READ;end
    PAIR_READ:begin pair_word<=pairs[pair_index];state<=PAIR_WAIT;end
    PAIR_WAIT:state<=PAIR_LATCH;
    PAIR_LATCH:begin ar<=phase_value_a[15:0];ai<=phase_value_a[31:16];br<=phase_value_b[15:0];bi<=phase_value_b[31:16];state<=PAIR_MULT;end
    PAIR_MULT:begin cross_re<=$signed(rr)+$signed(ii);cross_im<=$signed(ir)-$signed(ri);pair_weight<=pair_weight_product>>15;state<=PAIR_ACC;end
    PAIR_ACC:begin
     // A disabled channel may have unread/uninitialized phase RAM. Do not
     // multiply that data by zero and let X propagate into a valid group.
     if(pair_weight!=0)begin sum_re<=sum_re+(contribution_re>>>15);sum_im<=sum_im+(contribution_im>>>15);weight_sum<=weight_sum+pair_weight;end
     state<=PAIR_NEXT;
    end
    PAIR_NEXT:if(pair_index==pair_offset+pair_count-1)state<=AVG_START;else begin pair_index<=pair_index+1;state<=PAIR_READ;end
    AVG_START:if(weight_sum==0)begin bin_weight<=0;state<=BIN_SAVE;end else state<=AVG_WAIT;
    AVG_WAIT:if(ratio_done)begin mean_re<=normalized_re;mean_im<=normalized_im;state<=SQRT_START;end
    SQRT_START:state<=SQRT_WAIT;
    SQRT_WAIT:if(sqrt_done)begin bin_weight<=bin_weight_product>>15;if(magnitude==0||bin_weight_product==0)state<=BIN_SAVE;else state<=UNIT_START;end
    UNIT_START:state<=UNIT_WAIT;
    UNIT_WAIT:if(ratio_done)state<=BIN_SAVE;
    BIN_SAVE:begin
     reliable_sum<=reliable_sum+bin_weight;state<=BIN_NEXT;
    end
    BIN_NEXT:if(bin_index==BINS-1)state<=REL_START;else begin bin_index<=bin_index+1;state<=BIN_INIT;end
    REL_START:state<=REL_WAIT;
    REL_WAIT:if(rel_done)begin
     group_quality_index<=group_index;group_quality_valid<=1;
     if(reliable_fraction<4915||reliable_sum==0)begin group_quality_q15<=0;valid_groups[group_index]<=0;state<=GROUP_NEXT;end
     else begin group_quality_q15<={1'b0,reliable_fraction};valid_groups[group_index]<=1;valid_group_count<=valid_group_count+1;state<=SCALE_START;end
    end
    SCALE_START:state<=SCALE_WAIT;
    SCALE_WAIT:if(scale_done)begin scale_q16<=scale_quotient[31:0];fft_index<=0;fft_frame_done<=0;state<=FFT_CONFIG;end
    FFT_CONFIG:if(config_ready)state<=FFT_READ;
    FFT_READ:state<=FFT_READ_WAIT;
    FFT_READ_WAIT:begin fft_word<=!retained_input?48'd0:(fft_index>=3401?{(-spectral_im),spectral_re}:spectrum_word);state<=FFT_SEND;end
    FFT_SEND:if(fft_ready)begin if(fft_index==4095)state<=FFT_DRAIN;else begin fft_index<=fft_index+1;state<=FFT_READ;end end
    FFT_DRAIN:if(fft_frame_done)state<=GROUP_NEXT;
    GROUP_NEXT:begin
     if(group_index==81)state<=FINISH;
     else begin group_index<=group_index+1;bin_index<=0;reliable_sum<=0;state<=BIN_INIT;end
    end
    FINISH:begin window_valid<=valid_group_count>=12&&!fft_protocol_error;busy<=0;done<=1;state<=IDLE;end
    default:begin state<=IDLE;busy<=0;window_valid<=0;end
   endcase
  end
 end
endmodule
