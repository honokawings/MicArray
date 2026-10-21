`timescale 1ns/1ps
// Independent localization acquisition: every physical frame becomes exactly
// 48 consecutive FCLK words. Transport/DMA ready never enters this module.
(* KEEP_HIERARCHY = "TRUE" *)
module localization_capture48 (
    input wire clk,
    input wire rst_n,
    input wire frame_event,
    input wire [1151:0] frame_data,
    output wire [31:0] sample_word,
    output wire sample_valid,
    output wire [5:0] sample_channel
);
    (* ASYNC_REG = "TRUE" *) reg frame_event_meta;
    (* ASYNC_REG = "TRUE" *) reg frame_event_sync;
    reg frame_event_previous;
    reg [1151:0] frame_samples;
    reg sending;
    reg [5:0] channel;
    wire [23:0] selected_sample = frame_samples[channel*24 +: 24];
    assign sample_word = {{8{selected_sample[23]}}, selected_sample};
    assign sample_valid = sending;
    assign sample_channel = channel;
    always @(posedge clk) begin
        if (!rst_n) begin
            frame_event_meta <= 1'b0;
            frame_event_sync <= 1'b0;
            frame_event_previous <= 1'b0;
            frame_samples <= 1152'd0;
            sending <= 1'b0;
            channel <= 6'd0;
        end else begin
            frame_event_meta <= frame_event;
            frame_event_sync <= frame_event_meta;
            frame_event_previous <= frame_event_sync;
            // The 48 kHz period is >2083 FCLK cycles; transmission needs 48.
            // The microphone bus has settled >976 ns before the WS edge.
            if (frame_event_sync && !frame_event_previous) begin
                frame_samples <= frame_data;
                sending <= 1'b1;
                channel <= 6'd0;
            end else if (sending) begin
                if (channel == 6'd47) sending <= 1'b0;
                else channel <= channel + 1'b1;
            end
        end
    end
endmodule
