`timescale 1ns/1ps
// HMP1 packets occupy CH7..47 high bytes only. LOC3 always has priority.
// Every chunk has a generation, acoustic frame, offset and CRC8; incomplete
// maps must never be displayed. No raw microphone PCM word is replaced.
module audio_spatial_sideband (
 input wire clk,rst_n,
 input wire [31:0] location_transport_word,
 input wire audio_valid,audio_ready,input wire [5:0] channel_index,
 input wire location_group_reserved,
 input wire map_write_valid,input wire [10:0] map_write_index,
 input wire [7:0] map_write_value,
 input wire map_ready,map_invalidate,input wire [2:0] source_valid,
 input wire [31:0] locator_frame_counter,input wire [15:0] max_confidence_q15,
 output wire [31:0] transport_word
);
 (* ram_style="block" *) reg [7:0] map_bytes[0:2047];
 reg [10:0] read_address;reg [7:0] read_value;
 always @(posedge clk)begin
  if(map_write_valid)map_bytes[map_write_index]<=map_write_value;
  read_value<=map_bytes[read_address];
 end
 reg [2:0] state;
 localparam IDLE=0,HEADER=1,READ_REQUEST=2,READ_WAIT=3,READ_STORE=4,CRC_INIT=5,CRC_STEP=6,PREPARED=7;
 reg stream_active,prepared,group_active;
 reg [15:0] map_sequence,frame;
 reg [2:0] mask;reg [4:0] confidence_bucket;
 reg [10:0] offset;
 reg [5:0] length,byte_index,crc_index;
 reg [7:0] checksum;
 reg [327:0] prepared_packet,active_packet;
 reg [15:0] active_sequence;
 wire fire=audio_valid&&audio_ready;
 wire start_group=fire&&channel_index==0&&prepared&&!location_group_reserved;
 wire use_group=group_active||start_group;
 wire [327:0] selected_packet=start_group?prepared_packet:active_packet;
 wire [5:0] packet_byte_index=channel_index-6'd7;
 wire [8:0] packet_bit_index={packet_byte_index,3'b000};
 wire [7:0] sideband_byte=use_group&&channel_index>=7?
  selected_packet[packet_bit_index+:8]:location_transport_word[31:24];
 assign transport_word={sideband_byte,location_transport_word[23:0]};
 function [7:0] crc8(input [7:0] previous,input [7:0] value);
  reg [7:0] c;integer b;
  begin c=previous^value;for(b=0;b<8;b=b+1)c=c[7]?(c<<1)^8'h07:c<<1;crc8=c;end
 endfunction
 always @(posedge clk)begin
  if(!rst_n)begin
   state<=IDLE;stream_active<=0;prepared<=0;group_active<=0;
   map_sequence<=0;frame<=0;mask<=0;confidence_bucket<=0;offset<=0;length<=0;byte_index<=0;
   crc_index<=0;checksum<=0;prepared_packet<=0;active_packet<=0;
   active_sequence<=0;read_address<=0;
  end else begin
   if(start_group)begin
    group_active<=1;active_packet<=prepared_packet;active_sequence<=map_sequence;prepared<=0;
   end
   if(fire&&channel_index==47&&group_active)begin
    group_active<=0;
    if(active_sequence==map_sequence&&!map_ready&&!map_invalidate)begin
     if(mask==0||offset+length>=1369)begin stream_active<=0;state<=IDLE;end
     else begin offset<=offset+length;state<=HEADER;end
    end
   end
   if(map_invalidate)begin
    stream_active<=0;prepared<=0;state<=IDLE;
   end else if(map_ready)begin
    map_sequence<=map_sequence+1;frame<=locator_frame_counter[15:0];mask<=source_valid;confidence_bucket<=max_confidence_q15[14:10];
    offset<=0;stream_active<=1;prepared<=0;state<=HEADER;
   end else begin
    case(state)
     IDLE:begin end
     HEADER:begin
      prepared_packet<=0;
      prepared_packet[0+:32]<=32'h31504d48; // HMP1
      prepared_packet[32+:8]<=8'd1;
      prepared_packet[40+:8]<={mask==0?5'd0:confidence_bucket,mask};
      prepared_packet[48+:16]<=map_sequence;
      prepared_packet[64+:16]<=frame;
      prepared_packet[80+:16]<={5'd0,offset};
      prepared_packet[96+:8]<=mask==0?0:(1369-offset>25?25:1369-offset);
      prepared_packet[104+:8]<=37;prepared_packet[112+:8]<=37;
      length<=mask==0?0:(1369-offset>25?25:1369-offset);
      byte_index<=0;state<=mask==0?CRC_INIT:READ_REQUEST;
     end
     READ_REQUEST:begin read_address<=offset+byte_index;state<=READ_WAIT;end
     READ_WAIT:state<=READ_STORE;
     READ_STORE:begin
      prepared_packet[(15+byte_index)*8+:8]<=read_value;
      if(byte_index+1>=length)state<=CRC_INIT;else begin byte_index<=byte_index+1;state<=READ_REQUEST;end
     end
     CRC_INIT:begin crc_index<=0;checksum<=0;state<=CRC_STEP;end
     CRC_STEP:begin
      checksum<=crc8(checksum,prepared_packet[crc_index*8+:8]);
      if(crc_index==39)begin prepared_packet[320+:8]<=crc8(checksum,prepared_packet[312+:8]);prepared<=1;state<=PREPARED;end
      else crc_index<=crc_index+1;
     end
     PREPARED:begin end
    endcase
   end
  end
 end
endmodule
