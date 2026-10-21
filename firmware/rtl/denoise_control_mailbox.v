`timescale 1ns/1ps
// CRC-protected, idempotent commands over the existing six signed16 controls.
// A staged set is acknowledged active only after the beam bank reports applied.
module denoise_control_mailbox (
 input wire clk,rst_n,
 input wire [15:0] ctrl0,ctrl1,ctrl2,ctrl3,ctrl4,ctrl5,
 input wire cfg_busy,cfg_applied,
 output reg cfg_write,output reg [2:0] cfg_index,
 output reg signed [31:0] cfg_value,
 output reg cfg_commit,output reg [15:0] cfg_id,
 output reg stream_reset,
 output reg [15:0] ack_transaction,ack_field,
 output reg [31:0] invalid_count,output reg [15:0] error_code
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
 reg [5:0] field_mask;
 reg signed [31:0] staged_values[0:5];
 wire [15:0] command_id=command[31:16];
 wire [15:0] command_field=command[47:32];
 wire signed [31:0] command_value=command[79:48];
 function [15:0] crc16(input [15:0] old,input [7:0] value);
  integer bit_index;reg [15:0] c;
  begin c=old^{value,8'd0};for(bit_index=0;bit_index<8;bit_index=bit_index+1)
   c=c[15]?(c<<1)^16'h1021:c<<1;crc16=c;end
 endfunction
 integer k;
 always @(posedge clk)begin
  if(!rst_n)begin
   state<=WAIT_STABLE;previous<=0;processed<=0;processed_valid<=0;command<=0;
   stable_count<=0;crc_index<=0;checksum<=16'hffff;
   transaction_open<=0;commit_pending<=0;commit_seen_busy<=0;staging_id<=0;applied_id<=0;applied_id_valid<=0;field_mask<=0;
   cfg_write<=0;cfg_index<=0;cfg_value<=0;cfg_commit<=0;cfg_id<=0;stream_reset<=0;
   ack_transaction<=0;ack_field<=16'hffff;invalid_count<=0;error_code<=0;
   for(k=0;k<6;k=k+1)staged_values[k]<=0;
  end else begin
   cfg_write<=0;cfg_commit<=0;stream_reset<=0;
   if(observed!=previous)begin previous<=observed;stable_count<=0;end
   else if(stable_count!=63)stable_count<=stable_count+1;
   if(commit_pending&&cfg_busy)commit_seen_busy<=1;
   if(cfg_applied&&commit_pending)begin
    ack_transaction<=staging_id;ack_field<=16'h0101;error_code<=0;
    applied_id<=staging_id;applied_id_valid<=1;commit_pending<=0;commit_seen_busy<=0;transaction_open<=0;
   end else if(commit_pending&&commit_seen_busy&&!cfg_busy)begin
    // Invalid geometry may be rejected by the coefficient builder without applied.
    commit_pending<=0;commit_seen_busy<=0;transaction_open<=0;
    invalid_count<=invalid_count+1;error_code<=7;
   end
   case(state)
    WAIT_STABLE:if(stable_count>=31&&observed[15:0]==16'h4e44&&
                    (!processed_valid||observed!=processed))begin
     command<=observed;processed<=observed;processed_valid<=1;
     checksum<=16'hffff;crc_index<=0;state<=CRC_RUN;
    end
    CRC_RUN:begin
     checksum<=crc16(checksum,command[crc_index*8+:8]);
     if(crc_index==9)state<=DECODE;else crc_index<=crc_index+1;
    end
    DECODE:begin
     state<=WAIT_STABLE;
     if(checksum!=command[95:80])begin invalid_count<=invalid_count+1;error_code<=1;end
     else if(command_field==16'h0102)begin
      // Explicit reset is the recovery path and can abort a busy/rejected build.
      stream_reset<=1;transaction_open<=0;commit_pending<=0;commit_seen_busy<=0;field_mask<=0;applied_id_valid<=0;
      ack_transaction<=command_id;ack_field<=16'h0102;error_code<=0;
     end else if(command_field==16'h0100)begin
      if(cfg_busy||commit_pending)begin invalid_count<=invalid_count+1;error_code<=2;end
      else begin
       // Repeated BEGIN does not erase fields already accepted in this transaction.
       if(!transaction_open||staging_id!=command_id)begin
        staging_id<=command_id;field_mask<=0;transaction_open<=1;
       end
       ack_transaction<=command_id;ack_field<=16'h0100;error_code<=0;
      end
     end else if(command_field<6)begin
      if(!transaction_open||staging_id!=command_id||cfg_busy||commit_pending)begin
       invalid_count<=invalid_count+1;error_code<=3;
      end else if(command_value>32'sd234881024||command_value< -32'sd234881024)begin
       // Slopes are bounded by 0.025*48000/343 <3.5 samples.
       invalid_count<=invalid_count+1;error_code<=4;
      end else begin
       cfg_write<=1;cfg_index<=command_field[2:0];cfg_value<=command_value;
       staged_values[command_field[2:0]]<=command_value;field_mask[command_field[2:0]]<=1;
       ack_transaction<=command_id;ack_field<=command_field;error_code<=0;
      end
     end else if(command_field==16'h0101)begin
      if(applied_id_valid&&command_id==applied_id&&!transaction_open)begin
       ack_transaction<=command_id;ack_field<=16'h0101;error_code<=0;
      end else if(commit_pending&&staging_id==command_id)begin
       // Wait for the actual bank switch. Do not acknowledge early.
      end else if(!transaction_open||staging_id!=command_id||field_mask!=6'h3f||cfg_busy)begin
       invalid_count<=invalid_count+1;error_code<=5;
      end else begin cfg_commit<=1;cfg_id<=command_id;commit_pending<=1;commit_seen_busy<=0;error_code<=0;end
     end else begin invalid_count<=invalid_count+1;error_code<=6;end
    end
    default:state<=WAIT_STABLE;
   endcase
  end
 end
endmodule
