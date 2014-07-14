//
// Copyright (C) 2012 - Cabin Programs, Ken Keller 
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>

#ifndef FALSE
#define FALSE (0)
#define TRUE (!(FALSE))
#endif

#define sizeEEPROM 244
unsigned char eeprom[sizeEEPROM+100];	// Give a little extra for no good reason



int eepromIndex[2][46] = {
	{ -1, -1, 140, 142, 132, 134, 170, 176, 172, 174, 146, 144, 118, 120,
		150, 148, 122, 168, 116, 166, 164, 138, 136, 130, 128, 162, 202, 206,
		204, 208, 102, 104, 100, 200, 98, 198, 194, 196, 190, 192, 186, 188,
		182, 184, 178, 180 },
	{ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 124, 160, 126, 156, 152, 158,
		94, 92, 106, 108, 90, 88, 154, 112, 220, 110, 216, 214, 210, 212, 218,
		-1, 230, -1, 234, 232, 226, 228, 222, 224, 114, 96, -1, -1, -1, -1 }
};

int main(int argc, char* argv[])
{
	int currentnum, strnum, numberPin;
	char keypressed;
	char buffer[100];
	int index;

	printf("\n\n---EEPROM MAKER---\n\nThis is a program to make the EEPROM data file for a BeagleBone Cape.\n");
	printf("\nThis program produces an output file named: data.eeprom\n");
	printf("The data file follows EEPROM Format Revision 'A0'\n");
	printf("This data file can be put in the BeagleBone EEPROM by this command on a BeagleBone:\n");
	printf("   > cat data.eeprom >/sys/bus/i2c/drivers/at24/3-005x/eeprom\n");
	printf("         Where:  5x is 54, 55, 56, 57 depending on Cape addressing.\n");
	printf("         NOTE:  See blog.azkeller.com for more details.\n");
	printf("\n+++ No warranties or support is implied - sorry for CYA +++\n\n");

	for(index=0; index<sizeEEPROM; index++)
		eeprom[index]=0x00;

	eeprom[0] = 0xaa;
	eeprom[1] = 0x55;
	eeprom[2] = 0x33;
	eeprom[3] = 0xee;
	eeprom[4] = 0x41;
	eeprom[5] = 0x30;

	strcpy(buffer,"Zigbeag CC2520 Cape");
	strnum = strlen(buffer);
	if (strnum>32) strnum=32;
	for(index=0; index<strnum; index++)
		eeprom[6+index]=buffer[index];

	strcpy(buffer,"00A0");
	strnum = strlen(buffer);
	if (strnum>4) strnum=4;
	for(index=0; index<strnum; index++)
		eeprom[38+index]=buffer[index];

	strcpy(buffer, "Lab11, N.Jackson");
	strnum = strlen(buffer);
	if (strnum>16) strnum=16;
	for(index=0; index<strnum; index++)
		eeprom[42+index]=buffer[index];

	strcpy(buffer, "BB-BONE-CC2520");
	strnum = strlen(buffer);
	if (strnum>16) strnum=16;
	for(index=0; index<strnum; index++)
		eeprom[58+index]=buffer[index];

	printf("Enter Serial Number in ASCII (max 12): ");
	gets(buffer);
	strnum = strlen(buffer);
	if (strnum>12) strnum=12;
	for(index=0; index<strnum; index++)
		eeprom[76+index]=buffer[index];

	//MAX Current (mA) on VDD_3V3EXP Used by Cape (Range 0 to 250mA):
	currentnum = 250;
	eeprom[236]=currentnum>>8;
	eeprom[237]=currentnum & 0xff;

	//MAX Current (mA) on VDD_5V Used by Cape (Range 0 to 1000mA):
	currentnum = 0;
	eeprom[238]=currentnum>>8;
	eeprom[239]=currentnum & 0xff;

	//MAX Current (mA) on SYS_5V Used by Cape (Range 0 to 250mA):
	currentnum = 0;
	eeprom[240]=currentnum>>8;
	eeprom[241]=currentnum & 0xff;

	//Current (mA) Supplied on VDD_5V by Cape (Range 0 to 65535mA):
	currentnum = 0;
	eeprom[242]=currentnum>>8;
	eeprom[243]=currentnum & 0xff;


	//Number of Pins Used by Cape (Range 0 to 74):
	numberPin = 12;
	eeprom[75]=numberPin;

	int connector[12] = {8,8,8,9,9,8,8,8,9,9,9,9};
	int pin[12] = {15,12,11,15,12,17,18,16,17,18,21,22};
	unsigned char upper[12] = {160,160,160,192,160,192,192,192,192,192,160,160};
	unsigned char lower[12] = {47,47,47,23,47,23,23,23,16,16,48,48};

	for(int i = 0; i < numberPin; ++i){
		eeprom[eepromIndex[connector[i]-8][pin[i]-1]] = upper[i];
		eeprom[eepromIndex[connector[i]-8][pin[i]-1]+1] = lower[i];
	}

	//  write data to file
	printf("\nCreating output file... ./data.eeprom\n\n");
	char *file = "data.eeprom";
	FILE *p = NULL;
	p = fopen(file, "w");
	if (p== NULL) {
		printf("Error in opening a file..", file);
		return(1);
	}
	fwrite(eeprom, sizeEEPROM, 1, p);
	fclose(p);

	return 0;
}

