create_ip -vlnv xilinx.com:ip:xfft -module_name logmmse_hop_fft_syn_fft_params_s_core
set_property -dict [list \
    CONFIG.aclken {true} \
    CONFIG.aresetn {true} \
    CONFIG.butterfly_type {use_luts} \
    CONFIG.channels {1} \
    CONFIG.complex_mult_type {use_mults_resources} \
    CONFIG.cyclic_prefix_insertion {0} \
    CONFIG.data_format {fixed_point} \
    CONFIG.implementation_options {radix_4_burst_io} \
    CONFIG.input_width {32} \
    CONFIG.memory_options_data {block_ram} \
    CONFIG.memory_options_hybrid {0} \
    CONFIG.memory_options_phase_factors {block_ram} \
    CONFIG.memory_options_reorder {block_ram} \
    CONFIG.number_of_stages_using_block_ram_for_data_and_phase_factors {0} \
    CONFIG.output_ordering {natural_order} \
    CONFIG.ovflo {0} \
    CONFIG.phase_factor_width {34} \
    CONFIG.rounding_modes {convergent_rounding} \
    CONFIG.run_time_configurable_transform_length {0} \
    CONFIG.scaling_options {unscaled} \
    CONFIG.target_clock_frequency {100} \
    CONFIG.throttle_scheme {nonrealtime} \
    CONFIG.transform_length {2048} \
    CONFIG.xk_index {0} \
] -objects [get_ips logmmse_hop_fft_syn_fft_params_s_core]
set_property generate_synth_checkpoint false [get_files logmmse_hop_fft_syn_fft_params_s_core.xci]
generate_target {synthesis simulation} [get_files logmmse_hop_fft_syn_fft_params_s_core.xci]
