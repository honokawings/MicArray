`timescale 1ns / 1ps

module audio_multisource_sideband #(
    parameter integer LOCATION_REPEAT_PACKETS = 4,
    parameter integer LOCATION_HEARTBEAT_GROUPS = 2400,
    parameter [7:0] LOCATION_PROTOCOL_VERSION = 8'd4
)(
    input  wire                 clk,
    input  wire                 rst_n,

    input  wire [31:0]          audio_word,
    input  wire                 audio_valid,
    input  wire                 audio_ready,
    input  wire [5:0]           channel_index,
    input  wire                 processed_sideband_enable,
    input  wire                 location_sideband_enable,

    input  wire signed [23:0]   processed_sample,
    input  wire                 processed_valid,

    input  wire                 locations_updated,
    input  wire [2:0]           source_valid,
    input  wire [15:0]          source0_x,
    input  wire [15:0]          source0_y,
    input  wire signed [15:0]   source0_azimuth_cdeg,
    input  wire signed [15:0]   source0_elevation_cdeg,
    input  wire [15:0]          source0_confidence_q15,
    input  wire [15:0]          source1_x,
    input  wire [15:0]          source1_y,
    input  wire signed [15:0]   source1_azimuth_cdeg,
    input  wire signed [15:0]   source1_elevation_cdeg,
    input  wire [15:0]          source1_confidence_q15,
    input  wire [15:0]          source2_x,
    input  wire [15:0]          source2_y,
    input  wire signed [15:0]   source2_azimuth_cdeg,
    input  wire signed [15:0]   source2_elevation_cdeg,
    input  wire [15:0]          source2_confidence_q15,
    input  wire [31:0]          locator_frame_counter,

    output wire [31:0]          transport_word,
    output wire                 processed_insert,
    output wire                 metadata_insert,
    output wire                 location_group_reserved
);

    reg [327:0] pending_location_packet;
    reg [327:0] active_location_packet;
    reg pending_location_valid;
    reg location_group_armed;
    reg location_group_active;
    reg [15:0] location_sequence;
    reg location_send_pending;
    reg [3:0] location_repeat_remaining;
    reg [11:0] location_heartbeat_groups;

    wire audio_fire = audio_valid && audio_ready;
    wire start_location_group = location_sideband_enable &&
                                location_group_armed &&
                                (channel_index == 6'd0);
    wire use_location_group = location_sideband_enable &&
                              (location_group_active || start_location_group);
    wire [327:0] selected_location_packet = start_location_group
        ? pending_location_packet : active_location_packet;
    wire [5:0] location_byte_index = channel_index - 6'd7;
    wire [8:0] location_bit_index = {location_byte_index, 3'b000};
    wire [7:0] location_byte =
        selected_location_packet[location_bit_index +: 8];

    reg [7:0] sideband_byte;
    always @* begin
        sideband_byte = audio_word[31:24];
        if (use_location_group && channel_index >= 6'd7 && channel_index < 6'd48)
            sideband_byte = location_byte;
    end
    assign transport_word = {sideband_byte, audio_word[23:0]};
    // Reserved compatibility ports do not enable an audio processing path.
    assign processed_insert = 1'b0;
    assign location_group_reserved = use_location_group;
    assign metadata_insert = use_location_group && channel_index >= 6'd7;

    function [327:0] make_location_packet;
        input [15:0] sequence_value;
        input [15:0] frame_value;
        input [2:0] valid_value;
        input [15:0] x0;
        input [15:0] y0;
        input [15:0] az0;
        input [15:0] el0;
        input [15:0] conf0;
        input [15:0] x1;
        input [15:0] y1;
        input [15:0] az1;
        input [15:0] el1;
        input [15:0] conf1;
        input [15:0] x2;
        input [15:0] y2;
        input [15:0] az2;
        input [15:0] el2;
        input [15:0] conf2;
        integer byte_number;
        reg [327:0] packet;
        reg [7:0] checksum;
        begin
            packet = 328'd0;
            packet[(0*8) +: 8] = 8'h4c; // L
            packet[(1*8) +: 8] = 8'h4f; // O
            packet[(2*8) +: 8] = 8'h43; // C
            packet[(3*8) +: 8] = 8'h33; // 3
            packet[(4*8) +: 8] = LOCATION_PROTOCOL_VERSION;
            packet[(5*8) +: 8] = {5'd0, valid_value};
            packet[(6*8) +: 8] = sequence_value[7:0];
            packet[(7*8) +: 8] = sequence_value[15:8];
            packet[(8*8) +: 8] = frame_value[7:0];
            packet[(9*8) +: 8] = frame_value[15:8];

            packet[(10*8) +: 8] = {valid_value[0], 5'd0, 2'd0};
            packet[(11*8) +: 8] = x0[7:0];
            packet[(12*8) +: 8] = x0[15:8];
            packet[(13*8) +: 8] = y0[7:0];
            packet[(14*8) +: 8] = y0[15:8];
            packet[(15*8) +: 8] = az0[7:0];
            packet[(16*8) +: 8] = az0[15:8];
            packet[(17*8) +: 8] = el0[7:0];
            packet[(18*8) +: 8] = el0[15:8];
            packet[(19*8) +: 8] = conf0[14:7];

            packet[(20*8) +: 8] = {valid_value[1], 5'd0, 2'd1};
            packet[(21*8) +: 8] = x1[7:0];
            packet[(22*8) +: 8] = x1[15:8];
            packet[(23*8) +: 8] = y1[7:0];
            packet[(24*8) +: 8] = y1[15:8];
            packet[(25*8) +: 8] = az1[7:0];
            packet[(26*8) +: 8] = az1[15:8];
            packet[(27*8) +: 8] = el1[7:0];
            packet[(28*8) +: 8] = el1[15:8];
            packet[(29*8) +: 8] = conf1[14:7];

            packet[(30*8) +: 8] = {valid_value[2], 5'd0, 2'd2};
            packet[(31*8) +: 8] = x2[7:0];
            packet[(32*8) +: 8] = x2[15:8];
            packet[(33*8) +: 8] = y2[7:0];
            packet[(34*8) +: 8] = y2[15:8];
            packet[(35*8) +: 8] = az2[7:0];
            packet[(36*8) +: 8] = az2[15:8];
            packet[(37*8) +: 8] = el2[7:0];
            packet[(38*8) +: 8] = el2[15:8];
            packet[(39*8) +: 8] = conf2[14:7];

            checksum = 8'h00;
            for (byte_number = 0; byte_number < 40;
                 byte_number = byte_number + 1)
                checksum = checksum ^ packet[(byte_number*8) +: 8];
            packet[(40*8) +: 8] = checksum;
            make_location_packet = packet;
        end
    endfunction

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pending_location_packet <= make_location_packet(
                16'd0, 16'd0, 3'b000,
                16'd0, 16'd0, 16'd0, 16'd0, 16'd0,
                16'd0, 16'd0, 16'd0, 16'd0, 16'd0,
                16'd0, 16'd0, 16'd0, 16'd0, 16'd0
            );
            active_location_packet <= 328'd0;
            pending_location_valid <= 1'b1;
            location_group_armed <= 1'b0;
            location_group_active <= 1'b0;
            location_sequence <= 16'd0;
            location_send_pending <= 1'b0;
            location_repeat_remaining <= 4'd0;
            location_heartbeat_groups <= 12'd0;
        end else begin
            if (locations_updated) begin
                location_sequence <= location_sequence + 1'b1;
                pending_location_packet <= make_location_packet(
                    location_sequence + 1'b1,
                    locator_frame_counter[15:0],
                    source_valid,
                    source0_x, source0_y,
                    source0_azimuth_cdeg, source0_elevation_cdeg,
                    source0_confidence_q15,
                    source1_x, source1_y,
                    source1_azimuth_cdeg, source1_elevation_cdeg,
                    source1_confidence_q15,
                    source2_x, source2_y,
                    source2_azimuth_cdeg, source2_elevation_cdeg,
                    source2_confidence_q15
                );
                pending_location_valid <= 1'b1;
                location_send_pending <= 1'b1;
                location_repeat_remaining <=
                    (LOCATION_REPEAT_PACKETS > 1)
                    ? LOCATION_REPEAT_PACKETS - 1 : 0;
                location_heartbeat_groups <= 12'd0;
            end

            // A LOC3 update is repeated on several complete 48-channel groups.
            // Between updates, resend the latest packet as a low-rate heartbeat
            // so the host can distinguish "no source" from a broken sideband.
            if (audio_fire && channel_index == 6'd47 && !locations_updated) begin
                if (!location_sideband_enable) begin
                    location_group_armed <= 1'b0;
                end else if (!location_group_armed && pending_location_valid) begin
                    if (location_send_pending) begin
                        location_group_armed <= 1'b1;
                        location_send_pending <= 1'b0;
                        location_heartbeat_groups <= 12'd0;
                    end else if (location_repeat_remaining != 0) begin
                        location_group_armed <= 1'b1;
                        location_repeat_remaining <=
                            location_repeat_remaining - 1'b1;
                        location_heartbeat_groups <= 12'd0;
                    end else if (location_heartbeat_groups >=
                                 LOCATION_HEARTBEAT_GROUPS-1) begin
                        location_group_armed <= 1'b1;
                        location_heartbeat_groups <= 12'd0;
                    end else begin
                        location_heartbeat_groups <=
                            location_heartbeat_groups + 1'b1;
                    end
                end
            end

            if (audio_fire && channel_index == 6'd0) begin
                if (location_sideband_enable && location_group_armed) begin
                    active_location_packet <= pending_location_packet;
                    location_group_armed <= 1'b0;
                    location_group_active <= 1'b1;
                end else begin
                    location_group_armed <= 1'b0;
                    location_group_active <= 1'b0;
                end
            end

            if (audio_fire && channel_index == 6'd47) begin
                location_group_active <= 1'b0;
            end
        end
    end

endmodule
