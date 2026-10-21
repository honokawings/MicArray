`timescale 1ns / 1ps

module top(

    inout           [14:0]  M1PS_DDR_ADDR,
    inout           [2:0]   M1PS_DDR_BA,
    inout                   M1PS_DDR_CASn,
    inout                   M1PS_DDR_CLK_N,
    inout                   M1PS_DDR_CLK_P,
    inout                   M1PS_DDR_CKE,
    inout                   M1PS_DDR_CSn,
    inout           [3:0]   M1PS_DDR_DM,
    inout           [31:0]  M1PS_DDR_DQ,
    inout           [3:0]   M1PS_DDR_DQS_N,
    inout           [3:0]   M1PS_DDR_DQS_P,
    inout                   M1PS_DDR_ODT,
    inout                   M1PS_DDR_RASn,
    inout                   M1PS_DDR_RESET,
    inout                   M1PS_DDR_WEn,

    inout                   FIXED_IO_0_ddr_vrn,
    inout                   FIXED_IO_0_ddr_vrp,
    inout           [53:0]  FIXED_IO_0_mio,
    inout                   FIXED_IO_0_ps_clk,
    inout                   FIXED_IO_0_ps_porb,
    inout                   FIXED_IO_0_ps_srstb,

    input                   FPGA_CLK25MHz,
    output                  FPGA_PL_LED0,
    output                  FPGA_PL_LED1,

    inout                   PL_I2C_SCL_RTCIC,
    inout                   PL_I2C_SDA_RTCIC,
    input                   PL_UART1_RX,
    output                  PL_UART1_TX,

    output                  F_USB0_RESETn,
    output                  F_USB1_RESETn,

    input                   PL_RTCIC_INT_L,
    output                  F_WIFI_RESETn,

    input                   FPGA_PCIe_CLK_P,
    input                   FPGA_PCIe_CLK_N,
    output          [3:0]   FPGA_PCIe_TX_N,
    output          [3:0]   FPGA_PCIe_TX_P,
    input           [3:0]   FPGA_PCIe_RX_N,
    input           [3:0]   FPGA_PCIe_RX_P,
    output                  FPGA_PCIe_Rstn,
    output                  FPGA_PCIe_CLKREQ,

    output          [23:0]  SCK,
    output          [23:0]  WS,
    input           [47:0]  SD

);

    assign FPGA_PCIe_CLKREQ = 1'b0;

    assign F_USB0_RESETn = 1'b1;
    assign F_USB1_RESETn = 1'b1;

    wire FCLK_ZYNQ_100MHz;

    reg [26:0] led_cnt = 27'd0;

    always @(posedge FCLK_ZYNQ_100MHz) begin
        led_cnt <= led_cnt + 1'b1;
    end

    assign FPGA_PL_LED0 = led_cnt[26];
    assign FPGA_PL_LED1 = led_cnt[25];

    wire [127:0] M_AXIS_MM2S_DMA_tdata;
    wire [15:0]  M_AXIS_MM2S_DMA_tkeep;
    wire         M_AXIS_MM2S_DMA_tlast;
    wire         M_AXIS_MM2S_DMA_tready;
    wire         M_AXIS_MM2S_DMA_tvalid;

    assign M_AXIS_MM2S_DMA_tready = 1'b1;

    wire [127:0] M_AXIS_MM2S_DMA1_tdata;
    wire [15:0]  M_AXIS_MM2S_DMA1_tkeep;
    wire         M_AXIS_MM2S_DMA1_tlast;
    wire         M_AXIS_MM2S_DMA1_tready;
    wire         M_AXIS_MM2S_DMA1_tvalid;

    assign M_AXIS_MM2S_DMA1_tready = 1'b1;

    wire [127:0] S_AXIS_DMA_tdata;
    wire [15:0]  S_AXIS_DMA_tkeep;
    wire         S_AXIS_DMA_tlast;
    wire         S_AXIS_DMA_tready;
    wire         S_AXIS_DMA_tvalid;

    assign S_AXIS_DMA_tdata  = 128'd0;
    assign S_AXIS_DMA_tkeep  = 16'hFFFF;
    assign S_AXIS_DMA_tlast  = 1'b0;
    assign S_AXIS_DMA_tvalid = 1'b0;

    wire [31:0] GPIO_I;
    wire [31:0] GPIO_O;

//    wire [12:0] BRAM_PORTA_0_addr;
//    wire        BRAM_PORTA_0_clk;
//    wire [31:0] BRAM_PORTA_0_din;
//    wire [31:0] BRAM_PORTA_0_dout;
//    wire        BRAM_PORTA_0_en;
//    wire        BRAM_PORTA_0_rst;
//    wire [3:0]  BRAM_PORTA_0_we;

    wire        user_link_up_0;

    wire        prog_full_1;

    assign F_WIFI_RESETn = GPIO_O[3];

    assign GPIO_I = {29'd0, PL_RTCIC_INT_L, 2'b00};

    design_1_wrapper u_design_1_wrapper (
        .CLK_IN_D_0_clk_n        (FPGA_PCIe_CLK_N),
        .CLK_IN_D_0_clk_p        (FPGA_PCIe_CLK_P),

        .FCLK_ZYNQ_100MHz        (FCLK_ZYNQ_100MHz),

        .DDR_addr                (M1PS_DDR_ADDR),
        .DDR_ba                  (M1PS_DDR_BA),
        .DDR_cas_n               (M1PS_DDR_CASn),
        .DDR_ck_n                (M1PS_DDR_CLK_N),
        .DDR_ck_p                (M1PS_DDR_CLK_P),
        .DDR_cke                 (M1PS_DDR_CKE),
        .DDR_cs_n                (M1PS_DDR_CSn),
        .DDR_dm                  (M1PS_DDR_DM),
        .DDR_dq                  (M1PS_DDR_DQ),
        .DDR_dqs_n               (M1PS_DDR_DQS_N),
        .DDR_dqs_p               (M1PS_DDR_DQS_P),
        .DDR_odt                 (M1PS_DDR_ODT),
        .DDR_ras_n               (M1PS_DDR_RASn),
        .DDR_reset_n             (M1PS_DDR_RESET),
        .DDR_we_n                (M1PS_DDR_WEn),

        .FIXED_IO_ddr_vrn        (FIXED_IO_0_ddr_vrn),
        .FIXED_IO_ddr_vrp        (FIXED_IO_0_ddr_vrp),
        .FIXED_IO_mio            (FIXED_IO_0_mio),
        .FIXED_IO_ps_clk         (FIXED_IO_0_ps_clk),
        .FIXED_IO_ps_porb        (FIXED_IO_0_ps_porb),
        .FIXED_IO_ps_srstb       (FIXED_IO_0_ps_srstb),
        .GPIO_I                  (GPIO_I),
        .GPIO_O                  (GPIO_O),

        .IIC_rtc_scl_io          (PL_I2C_SCL_RTCIC),
        .IIC_rtc_sda_io          (PL_I2C_SDA_RTCIC),

        .M_AXIS_MM2S_DMA1_tdata  (M_AXIS_MM2S_DMA1_tdata),
        .M_AXIS_MM2S_DMA1_tkeep  (M_AXIS_MM2S_DMA1_tkeep),
        .M_AXIS_MM2S_DMA1_tlast  (M_AXIS_MM2S_DMA1_tlast),
        .M_AXIS_MM2S_DMA1_tready (M_AXIS_MM2S_DMA1_tready),
        .M_AXIS_MM2S_DMA1_tvalid (M_AXIS_MM2S_DMA1_tvalid),

        .M_AXIS_MM2S_DMA_tdata   (M_AXIS_MM2S_DMA_tdata),
        .M_AXIS_MM2S_DMA_tkeep   (M_AXIS_MM2S_DMA_tkeep),
        .M_AXIS_MM2S_DMA_tlast   (M_AXIS_MM2S_DMA_tlast),
        .M_AXIS_MM2S_DMA_tready  (M_AXIS_MM2S_DMA_tready),
        .M_AXIS_MM2S_DMA_tvalid  (M_AXIS_MM2S_DMA_tvalid),

        .S_AXIS_DMA_tdata        (S_AXIS_DMA_tdata),
        .S_AXIS_DMA_tkeep        (S_AXIS_DMA_tkeep),
        .S_AXIS_DMA_tlast        (S_AXIS_DMA_tlast),
        .S_AXIS_DMA_tready       (S_AXIS_DMA_tready),
        .S_AXIS_DMA_tvalid       (S_AXIS_DMA_tvalid),

        .prog_full_1             (prog_full_1),

        .UART_0_rxd              (PL_UART1_RX),
        .UART_0_txd              (PL_UART1_TX),

        .pcie_7x_mgt_0_rxn       (FPGA_PCIe_RX_N),
        .pcie_7x_mgt_0_rxp       (FPGA_PCIe_RX_P),
        .pcie_7x_mgt_0_txn       (FPGA_PCIe_TX_N),
        .pcie_7x_mgt_0_txp       (FPGA_PCIe_TX_P),
        .pcie_rstn               (FPGA_PCIe_Rstn),
        .user_link_up_0          (user_link_up_0),
        .SCK                     (SCK),
        .WS                      (WS),
        .SD                      (SD)
    );

endmodule