`timescale 1ns/1ps
// 2048 original samples, DC removal/Hann, median-energy channel qualification,
// zero padding to 4096, standard L2 PHAT with -60 dB amplitude floor.
module robust_spectral_frontend(
 input wire clk,rst_n,start,
 output reg [31:0] sample_read_address,
 input wire signed [23:0] sample_read_value,
 input wire [5:0] quality_read_channel,
 output wire [15:0] quality_read_q15,
 output reg [47:0] qualified_channels,
 output reg [5:0] qualified_count,
 output reg phase_write_valid,
 output reg [15:0] phase_write_address,
 output reg [31:0] phase_write_value,
 output reg busy,done,window_valid,fft_protocol_error
);
 localparam N=2048,BINS=687,FIRST_BIN=9,LAST_BIN=695;
 localparam IDLE=0,MEAN_REQ=1,MEAN_WAIT=2,MEAN_ACC=3,HANN_REQ=4,HANN_WAIT=5,HANN_ACC=6,
  SORT=7,MEDIAN_LOG=8,QUALITY_REQ=9,QUALITY_DB=10,QUALITY_SAVE=11,FFT_CHANNEL=12,FFT_CONFIG=13,
  FFT_READ=14,FFT_READ_WAIT=15,FFT_SEND=16,FFT_DRAIN=17,MAX_SQRT_START=18,MAX_SQRT_WAIT=19,
  PHASE_READ=20,PHASE_READ_WAIT=21,PHASE_SQRT_START=22,PHASE_SQRT_WAIT=23,
  RATIO_START=24,RATIO_WAIT=25,PHASE_NEXT=26,FINISH=27,WINDOW_REQ=28,WINDOW_WAIT=29,WINDOW_SAVE=30,HANN_MUL=31,HANN_SQUARE=32;
 reg [5:0] state,ch;
 reg [10:0] sample_index;
 reg [11:0] fft_index;
 reg [9:0] bin_index;
 reg signed [47:0] sum;
 reg signed [24:0] mean;
 reg signed [24:0] channel_means[0:47];
 reg [63:0] energy_sum;
 reg [63:0] energies[0:47],sorted[0:47];
 reg [15:0] qualities[0:47];
 assign quality_read_q15=quality_read_channel<48?qualities[quality_read_channel]:16'd0;
 wire [23:0] window_read;
 wire [10:0] window_write_address=sample_index;
 wire [10:0] window_read_address=fft_index[10:0];
 (* rom_style="block" *) reg [16:0] hann[0:N-1];
 (* rom_style="distributed" *) reg [12:0] log_table[0:1023];
 initial begin $readmemh("rom/hann2048.mem",hann);$readmemh("rom/log2_mantissa_q12.mem",log_table);end
 reg [16:0] hann_value;
 wire signed [24:0] centered=$signed(sample_read_value)-mean;
 wire signed [42:0] window_product=centered*$signed({1'b0,hann_value});
 wire signed [24:0] window_full=window_product>>>16;
 wire [23:0] window_write_data=$signed(window_full)>>>1;
 robust_sdp_ram #(.WIDTH(24),.DEPTH(N))u_window(clk,rst_n&&state==WINDOW_SAVE,window_write_address,window_write_data,window_read_address,window_read);
 reg signed [24:0] energy_window;
 reg [49:0] energy_product;
 wire [63:0] next_energy=energy_sum+{14'd0,energy_product};
 reg [5:0] sort_i,sort_j;
 reg [63:0] median_energy;
 reg [5:0] median_exponent,energy_exponent;
 reg [12:0] median_log,energy_log;
 function [5:0] exponent(input [63:0] value);
  integer i;begin exponent=0;for(i=0;i<64;i=i+1)if(value[i])exponent=i;end
 endfunction
 function [9:0] mantissa(input [63:0] value,input [5:0] e);
  reg [73:0] shifted;begin shifted={value,10'd0}>>e;mantissa=shifted[9:0];end
 endfunction
 reg signed [31:0] relative_log;
 wire signed [47:0] db_product=relative_log*16'sd12330;
 wire signed [31:0] relative_db_q8=db_product>>>16;
 wire [31:0] abs_db=relative_db_q8[31]?-relative_db_q8:relative_db_q8;
 reg signed [23:0] fft_word;
 wire [47:0] fft_out;
 wire [15:0] fft_user;
 wire fft_out_valid,fft_out_last,fft_ready,config_ready;
 wire event_unexpected,event_missing,event_status,event_input,event_output;
 reg fft_frame_done;
 wire signed [23:0] out_real=fft_out[23:0],out_imag=fft_out[47:24];
 wire signed [47:0] out_r2=out_real*out_real,out_i2=out_imag*out_imag;
 wire [49:0] out_magnitude_squared={2'd0,out_r2}+{2'd0,out_i2};
 wire [47:0] spectrum_read;
 wire [9:0] spectrum_write_address=fft_user[11:0]-FIRST_BIN;
 robust_sdp_ram #(.WIDTH(48),.DEPTH(BINS))u_spectrum(clk,rst_n&&fft_out_valid&&fft_user[11:0]>=FIRST_BIN&&fft_user[11:0]<=LAST_BIN,spectrum_write_address,fft_out,bin_index,spectrum_read);
 reg [49:0] maximum_magnitude_squared;
 reg [24:0] weak_threshold;
 wire signed [23:0] re=spectrum_read[23:0],im=spectrum_read[47:24];
 wire signed [47:0] r2=re*re,i2=im*im;
 wire [49:0] magnitude_squared={2'd0,r2}+{2'd0,i2};
 wire sqrt_start=(state==MAX_SQRT_START)||(state==PHASE_SQRT_START);
 wire sqrt_busy,sqrt_done;
 wire [24:0] magnitude;
 wire [49:0] sqrt_input=(state==MAX_SQRT_START)?maximum_magnitude_squared:magnitude_squared;
 robust_isqrt #(.W(50)) u_sqrt(clk,rst_n,sqrt_start,sqrt_input,sqrt_busy,sqrt_done,magnitude);
 wire norm_start=state==RATIO_START;
 wire norm_busy,norm_done;
 wire signed [15:0] nr,ni;
 wire signed [63:0] re_extended={{40{re[23]}},re};
 wire signed [63:0] im_extended={{40{im[23]}},im};
 robust_complex_ratio u_ratio(clk,rst_n,norm_start,re_extended,im_extended,{39'd0,magnitude},norm_busy,norm_done,nr,ni);
 robust_fft4096 u_fft(
  .aclk(clk),.aresetn(rst_n),.s_axis_config_tdata(16'h1555),.s_axis_config_tvalid(state==FFT_CONFIG),.s_axis_config_tready(config_ready),
  .s_axis_data_tdata({24'd0,fft_word}),.s_axis_data_tvalid(state==FFT_SEND),.s_axis_data_tready(fft_ready),.s_axis_data_tlast(fft_index==4095),
  .m_axis_data_tdata(fft_out),.m_axis_data_tuser(fft_user),.m_axis_data_tvalid(fft_out_valid),.m_axis_data_tready(1'b1),.m_axis_data_tlast(fft_out_last),
  .event_frame_started(),.event_tlast_unexpected(event_unexpected),.event_tlast_missing(event_missing),.event_status_channel_halt(event_status),.event_data_in_channel_halt(event_input),.event_data_out_channel_halt(event_output));
 always @(posedge clk) begin
  if(!rst_n)begin
   state<=IDLE;ch<=0;sample_index<=0;fft_index<=0;bin_index<=0;sum<=0;mean<=0;energy_sum<=0;sample_read_address<=0;
   qualified_channels<=0;qualified_count<=0;busy<=0;done<=0;window_valid<=0;fft_protocol_error<=0;phase_write_valid<=0;
   fft_frame_done<=0;maximum_magnitude_squared<=0;sort_i<=0;sort_j<=0;
  end else begin
   done<=0;phase_write_valid<=0;
   if(event_unexpected||event_missing)fft_protocol_error<=1;
   if(fft_out_valid)begin
    if(fft_user[11:0]>=FIRST_BIN&&fft_user[11:0]<=LAST_BIN)begin
     if(out_magnitude_squared>maximum_magnitude_squared)maximum_magnitude_squared<=out_magnitude_squared;
    end
    if(fft_out_last)fft_frame_done<=1;
   end
   case(state)
    IDLE:if(start)begin busy<=1;window_valid<=0;qualified_channels<=0;qualified_count<=0;ch<=0;sample_index<=0;sum<=0;state<=MEAN_REQ;end
    MEAN_REQ:begin sample_read_address<=ch*N+sample_index;state<=MEAN_WAIT;end
    MEAN_WAIT:state<=MEAN_ACC;
    MEAN_ACC:begin
     sum<=sum+$signed(sample_read_value);
     if(sample_index==N-1)begin mean<=($signed(sum)+$signed(sample_read_value))>>>11;channel_means[ch]<=($signed(sum)+$signed(sample_read_value))>>>11;sample_index<=0;energy_sum<=0;state<=HANN_REQ;end
     else begin sample_index<=sample_index+1;state<=MEAN_REQ;end
    end
    HANN_REQ:begin sample_read_address<=ch*N+sample_index;hann_value<=hann[sample_index];state<=HANN_WAIT;end
    HANN_WAIT:state<=HANN_MUL;
    HANN_MUL:begin energy_window<=window_full;state<=HANN_SQUARE;end
    HANN_SQUARE:begin energy_product<=energy_window*energy_window;state<=HANN_ACC;end
    HANN_ACC:begin
     energy_sum<=next_energy;
     if(sample_index==N-1)begin
      energies[ch]<=next_energy>>11;sorted[ch]<=next_energy>>11;
      if(ch==47)begin sort_i<=0;sort_j<=0;state<=SORT;end
      else begin ch<=ch+1;sample_index<=0;sum<=0;state<=MEAN_REQ;end
     end else begin sample_index<=sample_index+1;state<=HANN_REQ;end
    end
    SORT:begin
     if(sorted[sort_j]>sorted[sort_j+1])begin sorted[sort_j]<=sorted[sort_j+1];sorted[sort_j+1]<=sorted[sort_j];end
     if(sort_j==46-sort_i)begin sort_j<=0;if(sort_i==46)begin state<=MEDIAN_LOG;end else sort_i<=sort_i+1;end
     else sort_j<=sort_j+1;
    end
    MEDIAN_LOG:begin
     median_energy<=sorted[24];median_exponent<=exponent(sorted[24]);median_log<=log_table[mantissa(sorted[24],exponent(sorted[24]))];ch<=0;
     if(sorted[24]<72)state<=FINISH;else state<=QUALITY_REQ;
    end
    QUALITY_REQ:begin energy_exponent<=exponent(energies[ch]);energy_log<=log_table[mantissa(energies[ch],exponent(energies[ch]))];state<=QUALITY_DB;end
    QUALITY_DB:begin relative_log<=($signed({1'b0,energy_exponent})-$signed({1'b0,median_exponent}))*32'sd4096+$signed({1'b0,energy_log})-$signed({1'b0,median_log});state<=QUALITY_SAVE;end
    QUALITY_SAVE:begin
     if(energies[ch]<72||relative_db_q8< -6144||relative_db_q8>4608)begin qualities[ch]<=0;qualified_channels[ch]<=0;end
     else begin qualities[ch]<=16'd32768-abs_db*4;qualified_channels[ch]<=1;qualified_count<=qualified_count+1;end
     if(ch==47)begin ch<=0;state<=FFT_CHANNEL;end else begin ch<=ch+1;state<=QUALITY_REQ;end
    end
    FFT_CHANNEL:begin
     if(qualified_count<32)state<=FINISH;
     else if(!qualified_channels[ch])begin if(ch==47)begin window_valid<=1;state<=FINISH;end else ch<=ch+1;end
     else begin mean<=channel_means[ch];sample_index<=0;state<=WINDOW_REQ;end
    end
    WINDOW_REQ:begin sample_read_address<=ch*N+sample_index;hann_value<=hann[sample_index];state<=WINDOW_WAIT;end
    WINDOW_WAIT:state<=WINDOW_SAVE;
    WINDOW_SAVE:begin
     if(sample_index==N-1)begin maximum_magnitude_squared<=0;fft_frame_done<=0;fft_index<=0;state<=FFT_CONFIG;end
     else begin sample_index<=sample_index+1;state<=WINDOW_REQ;end
    end
    FFT_CONFIG:if(config_ready)state<=FFT_READ;
    FFT_READ:state<=FFT_READ_WAIT;
    FFT_READ_WAIT:begin fft_word<=fft_index<N?window_read:24'sd0;state<=FFT_SEND;end
    FFT_SEND:if(fft_ready)begin if(fft_index==4095)state<=FFT_DRAIN;else begin fft_index<=fft_index+1;state<=FFT_READ;end end
    FFT_DRAIN:if(fft_frame_done)state<=MAX_SQRT_START;
    MAX_SQRT_START:state<=MAX_SQRT_WAIT;
    MAX_SQRT_WAIT:if(sqrt_done)begin weak_threshold<=magnitude/1000;bin_index<=0;state<=PHASE_READ;end
    PHASE_READ:state<=PHASE_READ_WAIT;
    PHASE_READ_WAIT:state<=PHASE_SQRT_START;
    PHASE_SQRT_START:state<=PHASE_SQRT_WAIT;
    PHASE_SQRT_WAIT:if(sqrt_done)begin
     if(magnitude==0||magnitude<=weak_threshold)begin phase_write_address<=ch*BINS+bin_index;phase_write_value<=0;phase_write_valid<=1;state<=PHASE_NEXT;end
     else state<=RATIO_START;
    end
    RATIO_START:state<=RATIO_WAIT;
    RATIO_WAIT:if(norm_done)begin phase_write_address<=ch*BINS+bin_index;phase_write_value<={ni,nr};phase_write_valid<=1;state<=PHASE_NEXT;end
    PHASE_NEXT:begin
     if(bin_index==BINS-1)begin
      if(ch==47)begin window_valid<=!fft_protocol_error;state<=FINISH;end
      else begin ch<=ch+1;state<=FFT_CHANNEL;end
     end else begin bin_index<=bin_index+1;state<=PHASE_READ;end
    end
    FINISH:begin if(qualified_count<32)window_valid<=0;busy<=0;done<=1;state<=IDLE;end
    default:begin state<=IDLE;busy<=0;window_valid<=0;end
   endcase
  end
 end
endmodule
