`timescale 1ns/1ps
module robust_direction_vector(
 input wire clk,rst_n,start,
 input wire signed [31:0] azimuth_q16,elevation_q16,
 output reg busy,done,
 output reg signed [24:0] ux_q23,uy_q23,uz_q23
);
 (* rom_style="block" *) reg [24:0] sin_table[0:1440];
 initial $readmemh("rom/sin_degree_1_16_q23.mem",sin_table);
 reg [2:0] state;
 reg az_negative,el_negative;
 reg [10:0] az_index,el_index;
 reg [11:0] az_fraction,el_fraction;
 reg signed [25:0] as0,as1,ac0,ac1,es0,es1,ec0,ec1;
 reg signed [24:0] sa,ca,se,ce;
 reg signed [25:0] delta_as,delta_ac,delta_es,delta_ec;
 wire [31:0] aa=azimuth_q16[31]?-azimuth_q16:azimuth_q16;
 wire [31:0] ee=elevation_q16[31]?-elevation_q16:elevation_q16;
 wire signed [38:0] asd=delta_as*$signed({1'b0,az_fraction});
 wire signed [38:0] acd=delta_ac*$signed({1'b0,az_fraction});
 wire signed [38:0] esd=delta_es*$signed({1'b0,el_fraction});
 wire signed [38:0] ecd=delta_ec*$signed({1'b0,el_fraction});
 wire signed [25:0] ais=as0+(asd>>>12);
 wire signed [25:0] eis=es0+(esd>>>12);
 wire signed [49:0] xp=sa*ce,zp=ca*ce;
 always @(posedge clk) begin
  if(!rst_n) begin busy<=0;done<=0;state<=0;ux_q23<=0;uy_q23<=0;uz_q23<=0;az_index<=0;el_index<=0;az_fraction<=0;el_fraction<=0;az_negative<=0;el_negative<=0;end
  else begin
   done<=0;
   case(state)
    0: if(start) begin
     az_index<=aa>=32'd5898240 ? 11'd1440 : aa[22:12];
     el_index<=ee>=32'd5898240 ? 11'd1440 : ee[22:12];
     az_fraction<=aa>=32'd5898240 ? 0 : aa[11:0];el_fraction<=ee>=32'd5898240 ? 0 : ee[11:0];
     az_negative<=azimuth_q16[31];el_negative<=elevation_q16[31];busy<=1;state<=1;
    end
    1: begin
     as0<=$signed({1'b0,sin_table[az_index]});as1<=$signed({1'b0,sin_table[az_index==1440?1440:az_index+1]});
     ac0<=$signed({1'b0,sin_table[1440-az_index]});ac1<=$signed({1'b0,sin_table[az_index==1440?0:1439-az_index]});
     es0<=$signed({1'b0,sin_table[el_index]});es1<=$signed({1'b0,sin_table[el_index==1440?1440:el_index+1]});
     ec0<=$signed({1'b0,sin_table[1440-el_index]});ec1<=$signed({1'b0,sin_table[el_index==1440?0:1439-el_index]});state<=2;
    end
    2:begin delta_as<=as1-as0;delta_ac<=ac1-ac0;delta_es<=es1-es0;delta_ec<=ec1-ec0;state<=4;end
    4: begin sa<=az_negative?-ais:ais;ca<=ac0+(acd>>>12);se<=el_negative?-eis:eis;ce<=ec0+(ecd>>>12);state<=3;end
    3: begin ux_q23<=xp>>>23;uy_q23<=se;uz_q23<=zp>>>23;busy<=0;done<=1;state<=0;end
    default: begin state<=0;busy<=0;end
   endcase
  end
 end
endmodule
