set root [file dirname [file normalize [info script]]]
foreach required {DENOISE_HLS_STAGE DENOISE_GOLDEN_ROOT DENOISE_COMPILED_SIM_LIB} {
    if {![info exists ::env($required)]} {error "Required environment variable: $required"}
}
set build_root [file normalize [file join $root ../../../build/hls]]
if {[info exists ::env(DENOISE_HLS_BUILD_ROOT)]} {set build_root [file normalize $::env(DENOISE_HLS_BUILD_ROOT)]}
cd $build_root
set stage $::env(DENOISE_HLS_STAGE)
if {$stage ni {bandpass agc}} {error "DENOISE_HLS_STAGE must be bandpass or agc"}
open_project hls_$stage
open_solution solution1
set golden [file normalize $::env(DENOISE_GOLDEN_ROOT)]
cosim_design -setup -rtl verilog -tool modelsim -argv "$golden 4096" -trace_level none -compiled_library_dir [file normalize $::env(DENOISE_COMPILED_SIM_LIB)]
# The 2020 HLS harness compiles a generic FIFO-monitor class even for these
# scalar kernels, which contain no dataflow FIFO instance. Questa/ModelSim
# 2020.4 otherwise rejects that unused virtual-interface type at elaboration.
# Permit only unmatched virtual interfaces; retain all RTL and output checks.
set simulation [file normalize hls_$stage/solution1/sim/verilog]
set script [file join $simulation cosim.modelsim.scr]
set handle [open $script r]
set content [read $handle]
close $handle
if {[regexp -all -line {^vsim\s} $content] != 1} {error "Unexpected simulator script; refusing to patch"}
file copy -force $script ${script}.original
regsub -line {^vsim\s} $content {vsim -permit_unmatched_virtual_intf } content
set handle [open $script w]
puts -nonewline $handle $content
close $handle
set handle [open [file join $simulation monitor_compatibility_note.txt] w]
puts $handle "Only the ModelSim load option -permit_unmatched_virtual_intf was added for the unused HLS FIFO-monitor class. Original SCR preserved; production RTL and C/output comparisons are unchanged."
close $handle
set before [pwd]
cd $simulation
source run_sim.tcl
cd $before
puts "POST_${stage}_MANUAL_RTL_COSIM_PASS"
export_design -rtl verilog -format ip_catalog -flow syn
puts "POST_${stage}_EXPORTED_SYNTH_PASS"
exit
