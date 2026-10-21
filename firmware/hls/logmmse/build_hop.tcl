set source_root [file dirname [file normalize [info script]]]
set build_root [file normalize [file join $source_root ../../../build/hls]]
if {[info exists ::env(DENOISE_HLS_BUILD_ROOT)]} {set build_root [file normalize $::env(DENOISE_HLS_BUILD_ROOT)]}
file mkdir $build_root
cd $build_root
open_project -reset loghop
set_top logmmse_hop
add_files [file join $source_root logmmse_hop.cpp] -cflags "-ffp-contract=off"
open_solution -reset solution1
set_part xc7z035ffg676-2
create_clock -period 10 -name clk
set_clock_uncertainty 1.0
config_compile -unsafe_math_optimizations=false -no_signed_zeros=false -pipeline_loops 0
csynth_design
puts "LOGMMSE_HOP_SYNTH_PASS"
exit
