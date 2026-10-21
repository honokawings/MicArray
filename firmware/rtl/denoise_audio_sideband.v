`timescale 1ns/1ps
// Adds synchronized float32 A/B1/B2 without replacing any raw PCM24 samples.
// LOC3 has priority. This module's reservation is passed into the HMP1 scheduler.
module denoise_audio_sideband #(
 parameter FIFO_BITS=8,parameter STATUS_GROUPS=256
)(
 input wire clk,rst_n,stream_reset,
 input wire [31:0] location_transport_word,
 input wire audio_valid,audio_ready,input wire [5:0] channel_index,
 input wire location_group_reserved,
 input wire beam_valid,input wire [31:0] beam_a,beam_b1,beam_b2,
 input wire [31:0] beam_sample_index,input wire [15:0] beam_config_id,
 input wire ready,cfg_busy,input wire [15:0] cfg_active_id,
 input wire [15:0] ack_transaction,ack_field,
 input wire [31:0] core_overflow_count,invalid_control_count,
 input wire [15:0] error_code,
 output wire [31:0] transport_word,output wire group_reserved,
 output reg [31:0] overflow_count
);
 localparam FIFO_DEPTH=1<<FIFO_BITS;
 (* ram_style="distributed" *) reg [143:0] fifo[0:FIFO_DEPTH-1];
 reg [FIFO_BITS-1:0] write_pointer,read_pointer;
 reg [FIFO_BITS:0] fifo_count;
 wire [FIFO_BITS-1:0] second_pointer=read_pointer+1'b1;
 wire [143:0] first_tuple=fifo[read_pointer],second_tuple=fifo[second_pointer];
 wire consecutive=second_tuple[127:96]==first_tuple[127:96]+1'b1&&second_tuple[143:128]==first_tuple[143:128];
 reg [327:0] prepared_packet,active_packet;
 reg prepared,prepared_status,group_active;
 reg [1:0] prepared_count;
 reg [1:0] state;
 localparam IDLE=0,CRC=1,PREPARED=2;
 reg [5:0] crc_index;reg [7:0] checksum;
 reg status_pending;reg [15:0] previous_ack_transaction,previous_ack_field;
 reg [15:0] snapshot_ack_transaction,snapshot_ack_field;
 reg [15:0] previous_config;
 reg previous_ready,previous_busy;
 reg [15:0] status_groups;
 reg [2:0] lone_age;
 reg [31:0] latest_sample;
 wire fire=audio_valid&&audio_ready;
 wire start_group=fire&&channel_index==0&&prepared&&!location_group_reserved;
 wire use_group=group_active||start_group;
 wire [327:0] selected_packet=start_group?prepared_packet:active_packet;
 wire [5:0] byte_index=channel_index-6'd7;
 wire [8:0] bit_index={byte_index,3'b000};
 wire pop=start_group&&!prepared_status;
 wire [1:0] pop_count=pop?prepared_count:0;
 wire push=beam_valid&&(fifo_count<FIFO_DEPTH||pop);
 assign group_reserved=use_group;
 assign transport_word={use_group&&channel_index>=7?selected_packet[bit_index+:8]:location_transport_word[31:24],location_transport_word[23:0]};
 function [7:0] crc8(input [7:0] old,input [7:0] value);
  reg [7:0] c;integer b;
  begin c=old^value;for(b=0;b<8;b=b+1)c=c[7]?(c<<1)^8'h07:c<<1;crc8=c;end
 endfunction
 always @(posedge clk)begin
  if(!rst_n)begin
   write_pointer<=0;read_pointer<=0;fifo_count<=0;overflow_count<=0;
   prepared_packet<=0;active_packet<=0;prepared<=0;prepared_status<=0;prepared_count<=0;group_active<=0;
   state<=IDLE;crc_index<=0;checksum<=0;status_pending<=1;status_groups<=0;lone_age<=0;latest_sample<=0;
   previous_ack_transaction<=0;previous_ack_field<=16'hffff;snapshot_ack_transaction<=0;snapshot_ack_field<=0;
   previous_config<=0;previous_ready<=0;previous_busy<=0;
  end else begin
   previous_ack_transaction<=ack_transaction;previous_ack_field<=ack_field;
   previous_config<=cfg_active_id;previous_ready<=ready;previous_busy<=cfg_busy;
   if(ack_transaction!=previous_ack_transaction||ack_field!=previous_ack_field||
      cfg_active_id!=previous_config||ready!=previous_ready||cfg_busy!=previous_busy)status_pending<=1;
   if(beam_valid)latest_sample<=beam_sample_index;
   if(push)begin
    fifo[write_pointer]<={beam_config_id,beam_sample_index,beam_b2,beam_b1,beam_a};
    write_pointer<=write_pointer+1'b1;
   end else if(beam_valid)overflow_count<=overflow_count+1;
   if(pop)read_pointer<=read_pointer+prepared_count;
   case({push,pop})
    2'b10:fifo_count<=fifo_count+1'b1;
    2'b01:fifo_count<=fifo_count-pop_count;
    2'b11:fifo_count<=fifo_count+1'b1-pop_count;
    default:begin end
   endcase
   if(fire&&channel_index==47)begin
    group_active<=0;
    if(status_groups<STATUS_GROUPS)status_groups<=status_groups+1'b1;
    if(status_groups>=STATUS_GROUPS-1)status_pending<=1;
    if(fifo_count==1&&lone_age!=7)lone_age<=lone_age+1;else if(fifo_count!=1)lone_age<=0;
   end
   if(start_group)begin
    active_packet<=prepared_packet;group_active<=1;prepared<=0;state<=IDLE;
    if(prepared_status)begin
     status_groups<=0;
     if(snapshot_ack_transaction==ack_transaction&&snapshot_ack_field==ack_field&&
        previous_config==cfg_active_id&&previous_ready==ready&&previous_busy==cfg_busy)status_pending<=0;
    end else lone_age<=0;
   end
   case(state)
    IDLE:if(!prepared)begin
     if(status_pending)begin
      prepared_packet<=0;
      prepared_packet[0+:32]<=32'h31534e44; // DNS1
      prepared_packet[32+:8]<=1;prepared_packet[40+:8]<=1;
      prepared_packet[48+:16]<=cfg_active_id;
      prepared_packet[64+:16]<=ack_transaction;prepared_packet[80+:16]<=ack_field;
      prepared_packet[96+:8]<={5'd0,(overflow_count!=0||core_overflow_count!=0||error_code!=0),cfg_busy,ready};
      prepared_packet[104+:32]<=latest_sample;
      prepared_packet[136+:32]<=overflow_count+core_overflow_count;
      prepared_packet[168+:32]<=invalid_control_count;
      prepared_packet[200+:32]<=32'h20261020;prepared_packet[232+:16]<=error_code;
      prepared_status<=1;prepared_count<=0;
      snapshot_ack_transaction<=ack_transaction;snapshot_ack_field<=ack_field;
      checksum<=0;crc_index<=0;state<=CRC;
     end else if(fifo_count>=2||(fifo_count==1&&lone_age>=4))begin
      prepared_packet<=0;
      prepared_packet[0+:32]<=32'h33445541; // AUD3
      prepared_packet[32+:8]<=1;prepared_packet[40+:8]<=8'h11;
      prepared_packet[48+:16]<=first_tuple[143:128];
      prepared_packet[64+:32]<=first_tuple[127:96];
      prepared_packet[96+:8]<=fifo_count>=2&&consecutive?2:1;
      prepared_packet[104+:96]<=first_tuple[95:0];
      if(fifo_count>=2&&consecutive)prepared_packet[200+:96]<=second_tuple[95:0];
      prepared_status<=0;prepared_count<=fifo_count>=2&&consecutive?2:1;
      checksum<=0;crc_index<=0;state<=CRC;
     end
    end
    CRC:begin
     checksum<=crc8(checksum,prepared_packet[crc_index*8+:8]);
     if(crc_index==39)begin prepared_packet[320+:8]<=crc8(checksum,prepared_packet[312+:8]);prepared<=1;state<=PREPARED;end
     else crc_index<=crc_index+1;
    end
    PREPARED:begin end
    default:begin prepared<=0;state<=IDLE;end
   endcase
   if(stream_reset)begin
    // The raw/LOC3/HMP1 path keeps running. Cancel only this processing stream.
    write_pointer<=0;read_pointer<=0;fifo_count<=0;overflow_count<=0;
    prepared<=0;state<=IDLE;status_pending<=1;status_groups<=0;lone_age<=0;latest_sample<=0;
    // Never truncate a packet already on the wire; it is a previous config epoch.
   end
  end
 end
endmodule
