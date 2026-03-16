/*
 * Wolfskralle Socket Housing
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * required hardware:
 *  - 4 x M3 x 16mm countersunk bolts
 *  - 4 x M3 nuts
 *
 */

$fn = 100;

height = 16 - 1.6 - 2;

mmx();

module mmx() {    
        
    translate([5,0,0]) cube([2,10,height], center=true);    
    
    difference() {
        
        union() {
        translate([-21,0,3.7]) cube([15,28,5], center=true);    
        cube([28,28,height], center=true);    
        }
        
        translate([10,0,-12.5+5.5]) cube([10,25/2,height], center=true);    
        
        translate([-4,0,0]) cube([10,25,15], center=true);    
        
        translate([0,0,-5]) cube([25,10,5], center=true);    
        
        translate([-5,16,-8]) cube([23,15,5], center=true);    
        translate([-5,-16,-8]) cube([23,15,5], center=true);    

        translate([-4.25,-0.5,3]) cube([16.5,14,20], center=true);
        
        //translate([-3,0,-10]) cube([19,19,20], center=true);

        translate([21/2, 12.5-3, -10]) cylinder(d=3.2, h=100);
        translate([21/2, -12.5+3, -10]) cylinder(d=3.2, h=100);
        
        translate([21/2-36, 12.5-3, -10]) cylinder(d=3.2, h=100);
        translate([21/2-36, -12.5+3, -10]) cylinder(d=3.2, h=100);
        
        cylinder(d=3.5, h=100);
        
    }
    
}
