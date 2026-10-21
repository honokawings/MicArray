`timescale 1ns / 1ps

module multisource_audio_pipeline #(
    parameter UPDATE_FRAMES = 2400,
    parameter CONTROL_DIRECTION_ROM = "srp_direction_rom.mem",
    parameter LOCATOR_DIRECTION_ROM = "srp_direction_angles_241x181.mem"
)(
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire [31:0]          capture_word,
    input  wire                 capture_valid,
    input  wire [5:0]           capture_channel,
    input  wire [31:0]          audio_word,
    input  wire                 audio_valid,
    input  wire                 audio_ready,
    input  wire [5:0]           channel_index,
    input  wire signed [15:0]   ctrl0,
    input  wire signed [15:0]   ctrl1,
    input  wire signed [15:0]   ctrl2,
    input  wire signed [15:0]   ctrl3,
    input  wire signed [15:0]   ctrl4,
    input  wire signed [15:0]   ctrl5,
    input  wire signed [15:0]   ctrl6,
    input  wire signed [15:0]   ctrl7,

    output wire [31:0]          transport_word,
    output wire                 tracks_updated,
    output wire [2:0]           track_valid,
    output wire [15:0]          source0_x,
    output wire [15:0]          source0_y,
    output wire signed [15:0]   source0_azimuth_cdeg,
    output wire signed [15:0]   source0_elevation_cdeg,
    output wire [15:0]          source0_confidence_q15,
    output wire signed [31:0]   source0_score,
    output wire [31:0]          locator_frame_counter,
    output wire [15:0]          active_control_sequence,
    output wire [15:0]          active_mode_flags,
    output wire [31:0]          clip_count,
    output wire                 delay_ready,
    output wire                 locator_busy,
    output wire                 fft_protocol_error
);

    wire candidate_updated;
    wire [2:0] candidate_valid;
    wire [15:0] candidate0_x;
    wire [15:0] candidate0_y;
    wire signed [15:0] candidate0_azimuth;
    wire signed [15:0] candidate0_elevation;
    wire [15:0] candidate0_confidence;
    wire signed [31:0] candidate0_score;
    wire signed [15:0] candidate0_dir_x;
    wire signed [15:0] candidate0_dir_y;
    wire [15:0] candidate1_x;
    wire [15:0] candidate1_y;
    wire signed [15:0] candidate1_azimuth;
    wire signed [15:0] candidate1_elevation;
    wire [15:0] candidate1_confidence;
    wire signed [31:0] candidate1_score;
    wire signed [15:0] candidate1_dir_x;
    wire signed [15:0] candidate1_dir_y;
    wire [15:0] candidate2_x;
    wire [15:0] candidate2_y;
    wire signed [15:0] candidate2_azimuth;
    wire signed [15:0] candidate2_elevation;
    wire [15:0] candidate2_confidence;
    wire signed [31:0] candidate2_score;
    wire signed [15:0] candidate2_dir_x;
    wire signed [15:0] candidate2_dir_y;

    // Real PL fractional-delay A/B1/B2. All raw PCM24 and localization stay intact.
    wire denoise_cfg_write,denoise_cfg_commit,denoise_cfg_busy,denoise_cfg_applied;
    wire denoise_stream_reset,denoise_ready;
    wire [2:0] denoise_cfg_index;
    wire signed [31:0] denoise_cfg_value;
    wire [15:0] denoise_cfg_id,denoise_active_id;
    wire [15:0] denoise_ack_transaction,denoise_ack_field,denoise_error_code;
    wire [31:0] denoise_invalid_controls,denoise_core_overflows,denoise_transport_overflows;
    wire denoise_beam_valid;
    wire [31:0] denoise_beam_a,denoise_beam_b1,denoise_beam_b2,denoise_sample_index;
    wire [15:0] denoise_sample_config;
    wire denoise_group_reserved;
    wire [31:0] denoise_transport_word;
    wire denoise_wiener_mode,denoise_wiener_fault,denoise_wiener_valid,denoise_wiener_ready;
    wire denoise_wiener_input_ready;
    wire [23:0] denoise_coefficient1,denoise_coefficient2;
    wire [15:0] denoise_stream_id;
    wire [31:0] denoise_wiener_sample,denoise_wiener_index,denoise_wiener_errors,denoise_wiener_frames;
    wire [1:0] denoise_processing_mode;
    wire denoise_full_post_mode=denoise_processing_mode==2;
    assign denoise_wiener_mode=denoise_processing_mode!=0;
    wire denoise_post_input_ready,denoise_post_valid,denoise_processed_ready,denoise_post_ready,denoise_post_fault;
    wire [31:0] denoise_post_sample,denoise_post_index,denoise_post_fault_bits,denoise_post_numeric_fault;
    wire [31:0] denoise_post_blocks,denoise_post_max_cycles;
    wire denoise_selected_valid=denoise_full_post_mode?denoise_post_valid:denoise_wiener_valid;
    wire [31:0] denoise_selected_sample=denoise_full_post_mode?denoise_post_sample:denoise_wiener_sample;
    wire [31:0] denoise_selected_index=denoise_full_post_mode?denoise_post_index:denoise_wiener_index;
    assign denoise_wiener_ready=denoise_full_post_mode?denoise_post_input_ready:denoise_processed_ready;
    wire denoise_selected_ready=denoise_ready&&!denoise_wiener_fault&&
        (!denoise_full_post_mode||(denoise_post_ready&&!denoise_post_fault));
    wire [15:0] denoise_selected_error=denoise_error_code!=0?denoise_error_code:
        (denoise_full_post_mode&&denoise_post_fault?16'h8000|denoise_post_fault_bits[14:0]:16'd0);
    wire [31:0] denoise_all_core_overflows=denoise_core_overflows+denoise_wiener_errors+
        (denoise_full_post_mode&&denoise_post_fault?32'd1:32'd0);
    denoise_control_mailbox_v3 u_denoise_control(
        .clk(clk),.rst_n(rst_n),.ctrl0(ctrl0),.ctrl1(ctrl1),.ctrl2(ctrl2),
        .ctrl3(ctrl3),.ctrl4(ctrl4),.ctrl5(ctrl5),
        .cfg_busy(denoise_cfg_busy),.cfg_applied(denoise_cfg_applied),
        .cfg_write(denoise_cfg_write),.cfg_index(denoise_cfg_index),.cfg_value(denoise_cfg_value),
        .cfg_commit(denoise_cfg_commit),.cfg_id(denoise_cfg_id),.stream_reset(denoise_stream_reset),
        .ack_transaction(denoise_ack_transaction),.ack_field(denoise_ack_field),
        .invalid_count(denoise_invalid_controls),.error_code(denoise_error_code),
        .processing_mode(denoise_processing_mode),.coefficient1_q16(denoise_coefficient1),.coefficient2_q16(denoise_coefficient2),
        .stream_id(denoise_stream_id));
    denoise_beam3 u_denoise_beams(
        .clk(clk),.rst_n(rst_n&&!denoise_stream_reset),
        .sample_word(capture_word),.sample_valid(capture_valid),.sample_channel(capture_channel),
        .cfg_write(denoise_cfg_write),.cfg_index(denoise_cfg_index),.cfg_value(denoise_cfg_value),
        .cfg_commit(denoise_cfg_commit),.cfg_id(denoise_cfg_id),
        .cfg_busy(denoise_cfg_busy),.cfg_applied(denoise_cfg_applied),.cfg_active_id(denoise_active_id),
        .beam_valid(denoise_beam_valid),.beam_a(denoise_beam_a),.beam_b1(denoise_beam_b1),.beam_b2(denoise_beam_b2),
        .beam_sample_index(denoise_sample_index),.beam_config_id(denoise_sample_config),
        .overflow_count(denoise_core_overflows),.ready(denoise_ready));
    wiener1024_fixed #(.WINDOW_FILE("denoise_sqrt_hann_q30.mem")) u_denoise_wiener(
        .clk(clk),.rst_n(rst_n&&!denoise_stream_reset&&denoise_wiener_mode),
        .in_valid(denoise_beam_valid&&denoise_wiener_mode),.in_ready(denoise_wiener_input_ready),
        .in_a(denoise_beam_a),.in_b1(denoise_beam_b1),.in_b2(denoise_beam_b2),.in_sample_index(denoise_sample_index),
        .coefficient1_q16(denoise_coefficient1),.coefficient2_q16(denoise_coefficient2),
        .out_valid(denoise_wiener_valid),.out_ready(denoise_wiener_ready),
        .out_sample(denoise_wiener_sample),.out_sample_index(denoise_wiener_index),
        .fault(denoise_wiener_fault),.error_count(denoise_wiener_errors),.frames_completed(denoise_wiener_frames));
    denoise_post_profile_v3 u_denoise_post(
        .clk(clk),.rst_n(rst_n&&!denoise_stream_reset&&denoise_full_post_mode),
        .in_valid(denoise_wiener_valid&&denoise_full_post_mode),.in_ready(denoise_post_input_ready),
        .in_sample(denoise_wiener_sample),.in_sample_index(denoise_wiener_index),
        .out_valid(denoise_post_valid),.out_ready(denoise_processed_ready&&denoise_full_post_mode),
        .out_sample(denoise_post_sample),.out_sample_index(denoise_post_index),
        .ready(denoise_post_ready),.fault(denoise_post_fault),.fault_bits(denoise_post_fault_bits),
        .log_numeric_fault(denoise_post_numeric_fault),.blocks_completed(denoise_post_blocks),
        .max_block_cycles(denoise_post_max_cycles));
    assign active_control_sequence = denoise_active_id;
    assign active_mode_flags = {11'd0,denoise_full_post_mode,denoise_wiener_mode,denoise_cfg_busy,1'b1,denoise_selected_ready};
    assign clip_count = denoise_all_core_overflows+denoise_transport_overflows;
    assign delay_ready = denoise_selected_ready;

    wire heatmap_write_valid,heatmap_ready,heatmap_invalidate;
    wire [10:0] heatmap_write_index;
    wire [7:0] heatmap_write_value;
    wire [31:0] location_transport_word;
    wire location_group_reserved;
    srp_phat_locator_3src #(
        .UPDATE_FRAMES(UPDATE_FRAMES),
        .DIRECTION_ROM(LOCATOR_DIRECTION_ROM)
    ) u_locator (
        .clk(clk),
        .rst_n(rst_n),
        .sample_data(capture_word),
        .sample_valid(capture_valid),
        .sample_ready(1'b1),
        .sample_channel(capture_channel),
        .location_valid(candidate_updated),
        .source_valid(candidate_valid),
        .location_x(candidate0_x),
        .location_y(candidate0_y),
        .azimuth_cdeg(candidate0_azimuth),
        .elevation_cdeg(candidate0_elevation),
        .confidence_q15(candidate0_confidence),
        .location_score(candidate0_score),
        .source0_dir_x_q15(candidate0_dir_x),
        .source0_dir_y_q15(candidate0_dir_y),
        .source1_x(candidate1_x),
        .source1_y(candidate1_y),
        .source1_azimuth_cdeg(candidate1_azimuth),
        .source1_elevation_cdeg(candidate1_elevation),
        .source1_confidence_q15(candidate1_confidence),
        .source1_score(candidate1_score),
        .source1_dir_x_q15(candidate1_dir_x),
        .source1_dir_y_q15(candidate1_dir_y),
        .source2_x(candidate2_x),
        .source2_y(candidate2_y),
        .source2_azimuth_cdeg(candidate2_azimuth),
        .source2_elevation_cdeg(candidate2_elevation),
        .source2_confidence_q15(candidate2_confidence),
        .source2_score(candidate2_score),
        .source2_dir_x_q15(candidate2_dir_x),
        .source2_dir_y_q15(candidate2_dir_y),
        .frame_counter(locator_frame_counter),
        .locator_busy(locator_busy),
        .fft_protocol_error(fft_protocol_error),
        .heatmap_write_valid(heatmap_write_valid),.heatmap_write_index(heatmap_write_index),
        .heatmap_write_value(heatmap_write_value),.heatmap_ready(heatmap_ready),.heatmap_invalidate(heatmap_invalidate)
    );

    // Publish each completed scan without voice identity, holdover, or smoothing.
    assign tracks_updated = candidate_updated;
    assign track_valid = candidate_valid;
    assign source0_x = candidate0_x;
    assign source0_y = candidate0_y;
    assign source0_azimuth_cdeg = candidate0_azimuth;
    assign source0_elevation_cdeg = candidate0_elevation;
    assign source0_confidence_q15 = candidate0_confidence;
    assign source0_score = candidate0_score;
    audio_multisource_sideband u_sideband (
        .clk(clk),
        .rst_n(rst_n),
        .audio_word(audio_word),
        .audio_valid(audio_valid),
        .audio_ready(audio_ready),
        .channel_index(channel_index),
        .processed_sideband_enable(1'b0),
        .location_sideband_enable(1'b1),
        .processed_sample(24'sd0),
        .processed_valid(1'b0),
        .locations_updated(tracks_updated),
        .source_valid(track_valid),
        .source0_x(source0_x),
        .source0_y(source0_y),
        .source0_azimuth_cdeg(source0_azimuth_cdeg),
        .source0_elevation_cdeg(source0_elevation_cdeg),
        .source0_confidence_q15(source0_confidence_q15),
        .source1_x(candidate1_x),
        .source1_y(candidate1_y),
        .source1_azimuth_cdeg(candidate1_azimuth),
        .source1_elevation_cdeg(candidate1_elevation),
        .source1_confidence_q15(candidate1_confidence),
        .source2_x(candidate2_x),
        .source2_y(candidate2_y),
        .source2_azimuth_cdeg(candidate2_azimuth),
        .source2_elevation_cdeg(candidate2_elevation),
        .source2_confidence_q15(candidate2_confidence),
        .locator_frame_counter(locator_frame_counter),
        .transport_word(location_transport_word),
        .processed_insert(),
        .metadata_insert(),.location_group_reserved(location_group_reserved)
    );

    wire [15:0] heatmap_conf01=candidate0_confidence>candidate1_confidence?candidate0_confidence:candidate1_confidence;
    wire [15:0] heatmap_confidence=heatmap_conf01>candidate2_confidence?heatmap_conf01:candidate2_confidence;
    denoise_audio_sideband_v3 u_denoise_transport(
        .clk(clk),.rst_n(rst_n),.stream_reset(denoise_stream_reset),
        .location_transport_word(location_transport_word),
        .audio_valid(audio_valid),.audio_ready(audio_ready),.channel_index(channel_index),
        .location_group_reserved(location_group_reserved),
        .beam_valid(denoise_beam_valid),.beam_a(denoise_beam_a),.beam_b1(denoise_beam_b1),.beam_b2(denoise_beam_b2),
        .beam_sample_index(denoise_sample_index),.beam_config_id(denoise_sample_config),
        .wiener_mode(denoise_wiener_mode),.full_post_mode(denoise_full_post_mode),.stream_id(denoise_stream_id),
        .processed_valid(denoise_selected_valid),.processed_ready(denoise_processed_ready),
        .processed_sample(denoise_selected_sample),.processed_sample_index(denoise_selected_index),.wiener_frames(denoise_wiener_frames),
        .ready(denoise_selected_ready),.cfg_busy(denoise_cfg_busy),.cfg_active_id(denoise_active_id),
        .ack_transaction(denoise_ack_transaction),.ack_field(denoise_ack_field),
        .core_overflow_count(denoise_all_core_overflows),.invalid_control_count(denoise_invalid_controls),.error_code(denoise_selected_error),
        .transport_word(denoise_transport_word),.group_reserved(denoise_group_reserved),.overflow_count(denoise_transport_overflows));
    audio_spatial_sideband u_heatmap(
        .clk(clk),.rst_n(rst_n),.location_transport_word(denoise_transport_word),
        .audio_valid(audio_valid),.audio_ready(audio_ready),.channel_index(channel_index),
        .location_group_reserved(location_group_reserved||denoise_group_reserved),
        .map_write_valid(heatmap_write_valid),.map_write_index(heatmap_write_index),.map_write_value(heatmap_write_value),
        .map_ready(heatmap_ready),.map_invalidate(heatmap_invalidate),.source_valid(track_valid),
        .locator_frame_counter(locator_frame_counter),.max_confidence_q15(heatmap_confidence),.transport_word(transport_word));
endmodule
