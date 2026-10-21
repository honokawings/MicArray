`timescale 1ns/1ps
// Candidate v2 only. Commit ACK means the real beam bank was applied.
// Wiener mode/coefficients are frozen for a stream; RESET is explicit.
module denoise_control_mailbox_v2 (
 input wire clk,rst_n,
 input wire [15:0] ctrl0,ctrl1,ctrl2,ctrl3,ctrl4,ctrl5,
 input wire cfg_busy,cfg_applied,
 output reg cfg_write,output reg [2:0] cfg_index,
 output reg signed [31:0] cfg_value,
 output reg cfg_commit,output reg [15:0] cfg_id,
 output reg stream_reset,
 output reg [15:0] ack_transaction,ack_field,
 output reg [31:0] invalid_count,output reg [15:0] error_code,
 output reg wiener_mode,output reg [23:0] coefficient1_q16,coefficient2_q16,
 output reg [15:0] stream_id
);
 localparam WAIT_STABLE=0,CRC_RUN=1,DECODE=2;
 reg [1:0] state;
 wire [95:0] observed={ctrl5,ctrl4,ctrl3,ctrl2,ctrl1,ctrl0};
 reg [95:0] previous,processed,command;
 reg processed_valid;
 reg [5:0] stable_count;
 reg [3:0] crc_index;
 reg [15:0] checksum;
 reg transaction_open,commit_pending,commit_seen_busy;
 reg [15:0] staging_id,applied_id;
 reg applied_id_valid;
 reg [8:0] field_mask;
 reg staged_mode;
 reg [23:0] staged_coefficient1,staged_coefficient2;
 wire [15:0] command_id=command[31:16];
 wire [15:0] command_field=command[47:32];
 wire signed [31:0] command_value=command[79:48];
 wire parameters_changed=staged_mode!=wiener_mode ||
     staged_coefficient1!=coefficient1_q16 || staged_coefficient2!=coefficient2_q16;
 function [15:0] crc16(input [15:0] old,input [7:0] value);
  integer bit_index;reg [15:0] c;
  begin c=old^{value,8'd0};for(bit_index=0;bit_index<8;bit_index=bit_index+1)
   c=c[15]?(c<<1)^16'h1021:c<<1;crc16=c;end
 endfunction
 always @(posedge clk)begin
  if(!rst_n)begin
   state<=WAIT_STABLE;previous<=0;processed<=0;processed_valid<=0;command<=0;
   stable_count<=0;crc_index<=0;checksum<=16'hffff;
   transaction_open<=0;commit_pending<=0;commit_seen_busy<=0;staging_id<=0;applied_id<=0;applied_id_valid<=0;field_mask<=0;
   cfg_write<=0;cfg_index<=0;cfg_value<=0;cfg_commit<=0;cfg_id<=0;stream_reset<=0;
   ack_transaction<=0;ack_field<=16'hffff;invalid_count<=0;error_code<=0;
   staged_mode<=0;staged_coefficient1<=0;staged_coefficient2<=0;
   wiener_mode<=0;coefficient1_q16<=0;coefficient2_q16<=0;stream_id<=0;
  end else begin
   cfg_write<=0;cfg_commit<=0;stream_reset<=0;
   if(observed!=previous)begin previous<=observed;stable_count<=0;end
   else if(stable_count!=63)stable_count<=stable_count+1'b1;
   if(commit_pending&&cfg_busy)commit_seen_busy<=1;
   if(cfg_applied&&commit_pending)begin
    ack_transaction<=staging_id;ack_field<=16'h0101;error_code<=0;
    applied_id<=staging_id;applied_id_valid<=1;commit_pending<=0;commit_seen_busy<=0;transaction_open<=0;
    wiener_mode<=staged_mode;coefficient1_q16<=staged_coefficient1;coefficient2_q16<=staged_coefficient2;
    if(!applied_id_valid)stream_id<=staging_id;
   end else if(commit_pending&&commit_seen_busy&&!cfg_busy)begin
    commit_pending<=0;commit_seen_busy<=0;transaction_open<=0;
    invalid_count<=invalid_count+1'b1;error_code<=7;
   end
   case(state)
    WAIT_STABLE:if(stable_count>=31&&observed[15:0]==16'h4e44&&
                    (!processed_valid||observed!=processed))begin
     command<=observed;processed<=observed;processed_valid<=1;
     checksum<=16'hffff;crc_index<=0;state<=CRC_RUN;
    end
    CRC_RUN:begin
     checksum<=crc16(checksum,command[crc_index*8+:8]);
     if(crc_index==9)state<=DECODE;else crc_index<=crc_index+1'b1;
    end
    DECODE:begin
     state<=WAIT_STABLE;
     if(checksum!=command[95:80])begin invalid_count<=invalid_count+1'b1;error_code<=1;end
     else if(command_field==16'h0102)begin
      stream_reset<=1;transaction_open<=0;commit_pending<=0;commit_seen_busy<=0;field_mask<=0;applied_id_valid<=0;
      ack_transaction<=command_id;ack_field<=16'h0102;error_code<=0;
      staged_mode<=0;staged_coefficient1<=0;staged_coefficient2<=0;
      wiener_mode<=0;coefficient1_q16<=0;coefficient2_q16<=0;stream_id<=0;
     end else if(command_field==16'h0100)begin
      if(cfg_busy||commit_pending)begin invalid_count<=invalid_count+1'b1;error_code<=2;end
      else begin
       if(!transaction_open||staging_id!=command_id)begin
        staging_id<=command_id;field_mask<=0;transaction_open<=1;
        staged_mode<=wiener_mode;staged_coefficient1<=coefficient1_q16;staged_coefficient2<=coefficient2_q16;
       end
       ack_transaction<=command_id;ack_field<=16'h0100;error_code<=0;
      end
     end else if(command_field<9)begin
      if(!transaction_open||staging_id!=command_id||cfg_busy||commit_pending)begin
       invalid_count<=invalid_count+1'b1;error_code<=3;
      end else if(command_field<6)begin
       if(command_value>32'sd234881024||command_value< -32'sd234881024)begin
        invalid_count<=invalid_count+1'b1;error_code<=4;
       end else begin
        cfg_write<=1;cfg_index<=command_field[2:0];cfg_value<=command_value;
        field_mask[command_field[3:0]]<=1;
        ack_transaction<=command_id;ack_field<=command_field;error_code<=0;
       end
      end else if((command_field==6 && command_value!=0 && command_value!=1) ||
                  (command_field>=7 && (command_value<0 || command_value>32'sd6553600)))begin
       invalid_count<=invalid_count+1'b1;error_code<=4;
      end else begin
       if(command_field==6)staged_mode<=command_value[0];
       else if(command_field==7)staged_coefficient1<=command_value[23:0];
       else staged_coefficient2<=command_value[23:0];
       field_mask[command_field[3:0]]<=1;
       ack_transaction<=command_id;ack_field<=command_field;error_code<=0;
      end
     end else if(command_field==16'h0101)begin
      if(applied_id_valid&&command_id==applied_id&&!transaction_open)begin
       ack_transaction<=command_id;ack_field<=16'h0101;error_code<=0;
      end else if(commit_pending&&staging_id==command_id)begin
       // The command retry cannot restart a bank build or acknowledge it early.
      end else if(!transaction_open||staging_id!=command_id||field_mask[5:0]!=6'h3f||cfg_busy||
                  (staged_mode && field_mask!=9'h1ff) ||
                  (!staged_mode && field_mask[8:6]!=0 && field_mask[8:6]!=3'b111))begin
       invalid_count<=invalid_count+1'b1;error_code<=5;
      end else if(applied_id_valid&&parameters_changed)begin
       invalid_count<=invalid_count+1'b1;error_code<=8;
      end else begin cfg_commit<=1;cfg_id<=command_id;commit_pending<=1;commit_seen_busy<=0;error_code<=0;end
     end else begin invalid_count<=invalid_count+1'b1;error_code<=6;end
    end
    default:state<=WAIT_STABLE;
   endcase
  end
 end
endmodule
