set here [file dirname [file normalize [info script]]]
set rtl [file normalize [file join $here ../../hls_rtl/logmmse]]
set build [file normalize [file join $here ../../../build/log_math_ooc]]
if {[info exists ::env(DENOISE_LOG_OOC_BUILD_ROOT)]} {set build [file normalize $::env(DENOISE_LOG_OOC_BUILD_ROOT)]}
file mkdir $build
create_project -force logmath_project [file join $build project] -part xc7z035ffg676-2
set_property target_language Verilog [current_project]
foreach script [lsort [glob [file join $rtl *_ip.tcl]]] {source $script}
add_files [glob [file join $rtl *.v]]
add_files [glob -nocomplain [file join $rtl *.dat]]
set_property top logmmse_hop [current_fileset]
update_compile_order -fileset sources_1
set constraints [file join $build timing.xdc]
set fd [open $constraints w]
puts $fd {create_clock -period 10 -name clk [get_ports ap_clk]}
close $fd
add_files -fileset constrs_1 $constraints
synth_design -mode out_of_context -top logmmse_hop -part xc7z035ffg676-2
report_utilization -file [file join $build log_math_utilization_synth.rpt]
report_utilization -hierarchical -file [file join $build log_math_utilization_hierarchy.rpt]
set black [get_cells -quiet -hierarchical -filter {IS_BLACKBOX == 1}]
puts "LOG_MATH_BLACKBOX_COUNT [llength $black]"
if {[llength $black]} {error "Unresolved math/FFT primitives"}
write_checkpoint -force [file join $build logmmse_hop_synth.dcp]
write_verilog -force -mode synth_stub [file join $build logmmse_hop_stub.v]
puts "LOGMMSE_R5_DIRECT_MATH_SYNTH_PASS"
