`timescale 1ns / 1ps

module Audio4ch_To_MicStream_48ch #(
    parameter ACTIVE_CHANNELS = 48
)(
    input  wire               clk,
    input  wire               rst_n,
    input  wire               clk_48k,
    input  wire signed [15:0] ctrl0,

    input  wire signed [23:0] ch1,
    input  wire signed [23:0] ch2,
    input  wire signed [23:0] ch3,
    input  wire signed [23:0] ch4,
    input  wire signed [23:0] ch5,
    input  wire signed [23:0] ch6,
    input  wire signed [23:0] ch7,
    input  wire signed [23:0] ch8,
    input  wire signed [23:0] ch9,
    input  wire signed [23:0] ch10,
    input  wire signed [23:0] ch11,
    input  wire signed [23:0] ch12,
    input  wire signed [23:0] ch13,
    input  wire signed [23:0] ch14,
    input  wire signed [23:0] ch15,
    input  wire signed [23:0] ch16,
    input  wire signed [23:0] ch17,
    input  wire signed [23:0] ch18,
    input  wire signed [23:0] ch19,
    input  wire signed [23:0] ch20,
    input  wire signed [23:0] ch21,
    input  wire signed [23:0] ch22,
    input  wire signed [23:0] ch23,
    input  wire signed [23:0] ch24,
    input  wire signed [23:0] ch25,
    input  wire signed [23:0] ch26,
    input  wire signed [23:0] ch27,
    input  wire signed [23:0] ch28,
    input  wire signed [23:0] ch29,
    input  wire signed [23:0] ch30,
    input  wire signed [23:0] ch31,
    input  wire signed [23:0] ch32,
    input  wire signed [23:0] ch33,
    input  wire signed [23:0] ch34,
    input  wire signed [23:0] ch35,
    input  wire signed [23:0] ch36,
    input  wire signed [23:0] ch37,
    input  wire signed [23:0] ch38,
    input  wire signed [23:0] ch39,
    input  wire signed [23:0] ch40,
    input  wire signed [23:0] ch41,
    input  wire signed [23:0] ch42,
    input  wire signed [23:0] ch43,
    input  wire signed [23:0] ch44,
    input  wire signed [23:0] ch45,
    input  wire signed [23:0] ch46,
    input  wire signed [23:0] ch47,
    input  wire signed [23:0] ch48,

    output reg  [31:0]        mic_data_32,
    output reg                mic_data_valid,
    input  wire               mic_data_ready,
    output reg                mic_data_last,
    output reg  [5:0]         mic_channel_index
);

    // All state lives in the 100 MHz domain. clk_48k is synchronized data.
    // ICS43432 updates its parallel outputs about 976 ns before the WS rising
    // edge; two synchronizer stages provide additional bus-settling time.
    (* ASYNC_REG = "TRUE" *) reg frame_event_meta = 1'b0;
    (* ASYNC_REG = "TRUE" *) reg frame_event_sync = 1'b0;
    reg frame_event_previous = 1'b0;
    wire frame_event = frame_event_sync && !frame_event_previous;
    wire [1151:0] input_frame = {
                ch48,
                ch47,
                ch46,
                ch45,
                ch44,
                ch43,
                ch42,
                ch41,
                ch40,
                ch39,
                ch38,
                ch37,
                ch36,
                ch35,
                ch34,
                ch33,
                ch32,
                ch31,
                ch30,
                ch29,
                ch28,
                ch27,
                ch26,
                ch25,
                ch24,
                ch23,
                ch22,
                ch21,
                ch20,
                ch19,
                ch18,
                ch17,
                ch16,
                ch15,
                ch14,
                ch13,
                ch12,
                ch11,
                ch10,
                ch9,
                ch8,
                ch7,
                ch6,
                ch5,
                ch4,
                ch3,
                ch2,
                ch1
    };
    reg [1151:0] pending_frame;
    reg [1151:0] active_frame;
    reg pending_valid = 1'b0;
    reg sending = 1'b0;
    reg [5:0] send_ch = 6'd0;
    // If downstream stalls over two sample periods, discard an entire queued
    // frame in favor of the newest frame. Never mix samples within a frame.
    (* DONT_TOUCH = "TRUE" *) reg [31:0] overrun_count = 32'd0;
    wire output_slot_free = !mic_data_valid || mic_data_ready;
    wire consume_pending = !sending && output_slot_free && pending_valid;
    wire signed [23:0] selected_sample = active_frame[send_ch*24 +: 24];

    always @(posedge clk) begin
        if (!rst_n) begin
            frame_event_meta <= 1'b0;
            frame_event_sync <= 1'b0;
            frame_event_previous <= 1'b0;
            pending_valid <= 1'b0;
            sending <= 1'b0;
            send_ch <= 6'd0;
            overrun_count <= 32'd0;
            mic_data_32 <= 32'd0;
            mic_data_valid <= 1'b0;
            mic_data_last <= 1'b0;
            mic_channel_index <= 6'd48;
        end else begin
            frame_event_meta <= clk_48k;
            frame_event_sync <= frame_event_meta;
            frame_event_previous <= frame_event_sync;
            if (mic_data_valid && mic_data_ready) begin
                mic_data_valid <= 1'b0;
                mic_data_last <= 1'b0;
                mic_channel_index <= 6'd48;
            end
            if (sending && output_slot_free) begin
                mic_data_32 <= send_ch < ACTIVE_CHANNELS
                    ? {{8{selected_sample[23]}}, selected_sample} : 32'd0;
                mic_data_valid <= 1'b1;
                mic_data_last <= send_ch == 6'd47;
                mic_channel_index <= send_ch;
                if (send_ch == 6'd47) begin
                    sending <= 1'b0;
                    send_ch <= 6'd0;
                end else send_ch <= send_ch + 1'b1;
            end
            if (consume_pending) begin
                active_frame <= pending_frame;
                pending_valid <= 1'b0;
                sending <= 1'b1;
                send_ch <= 6'd0;
            end
            // This assignment follows consume_pending so a simultaneous sample
            // arrival and queue dequeue retains the newly captured frame.
            if (frame_event) begin
                pending_frame <= input_frame;
                pending_valid <= 1'b1;
                if (pending_valid && !consume_pending && overrun_count != 32'hffffffff)
                    overrun_count <= overrun_count + 1'b1;
            end
        end
    end
    // ctrl0 is retained solely for the existing block-design port contract.
    // No gain, subtraction, beamforming, or voice identification is performed.
endmodule
