set root [file dirname [file normalize [info script]]]
set build_root [file normalize [file join $root ../../../build/hls]]
if {[info exists ::env(DENOISE_HLS_BUILD_ROOT)]} {set build_root [file normalize $::env(DENOISE_HLS_BUILD_ROOT)]}
file mkdir $build_root
cd $build_root
set stage $::env(DENOISE_HLS_STAGE)
if {$stage ni {bandpass agc}} {error "DENOISE_HLS_STAGE must be bandpass or agc"}
set top denoise_${stage}64
set flags "-ffp-contract=off"
if {$stage eq "agc"} {append flags " -DAGC_TOP"}
open_project -reset hls_$stage
set_top $top
add_files [file join $root post_sample64.cpp] -cflags $flags
add_files -tb [file join $root post_sample64_tb.cpp] -cflags $flags
open_solution -reset solution1 -flow_target vivado
set_part xc7z035ffg676-2
create_clock -period 10 -name default
config_compile -unsafe_math_optimizations=false
config_rtl -reset all -reset_async=false
if {[info exists ::env(DENOISE_GOLDEN_ROOT)]} {
    csim_design -argv [file normalize $::env(DENOISE_GOLDEN_ROOT)]
} else {
    puts "Private audio fixtures absent: C simulation skipped (not a numeric PASS)."
}
csynth_design
puts "POST_${stage}_CSIM_AND_SYNTH_PASS"
exit
