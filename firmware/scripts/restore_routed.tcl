# Vivado 2020.2: restore the released, already-routed implementation.
# This does not synthesize edited RTL or demonstrate timing for a changed design.
if {$argc != 2} {error "Usage: vivado -mode batch -source restore_routed.tcl -tclargs routed.dcp output_dir"}
set checkpoint [file normalize [lindex $argv 0]]
set output [file normalize [lindex $argv 1]]
if {![file exists $checkpoint]} {error "Checkpoint not found: $checkpoint"}
file mkdir $output
open_checkpoint $checkpoint
if {[get_property PART [current_design]] ne "xc7z035ffg676-2"} {error "Unexpected FPGA part"}
report_route_status -file [file join $output route_status.rpt]
report_timing_summary -delay_type min_max -report_unconstrained -file [file join $output timing_summary.rpt]
report_utilization -file [file join $output utilization.rpt]
report_drc -file [file join $output drc.rpt]
set errors [get_drc_violations -quiet -filter {SEVERITY == Error}]
if {[llength $errors]} {error "DRC errors prevent bitstream recovery"}
write_bitstream -force [file join $output pl.bit]
puts "Recovered bitstream from released routed checkpoint. Check SHA256 and generated reports."
