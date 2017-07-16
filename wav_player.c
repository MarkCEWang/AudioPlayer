#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> //strcasecmp()
#include <stdint.h>
#include <unistd.h>
#include <system.h>
#include <sys/alt_alarm.h>
#include <io.h>
#include "fatfs.h"
#include "diskio.h"
#include "ff.h"
#include "monitor.h"
#include "uart.h"
#include "alt_types.h"
#include <altera_up_avalon_audio.h>
#include <altera_up_avalon_audio_and_video_config.h>
#include <altera_avalon_lcd_16207.h>
#include "altera_avalon_pio_regs.h" //button interrupts
#include "sys/alt_irq.h"

alt_up_audio_dev * audio_dev;
DIR Dir;
FIL File1, File2;
FILE *lcd;
int bufferSize = 64; //set up audio buffer
uint16_t Buffer[64];
uint32_t s2;
int fileOffset = 0;

int numSongs = 0;
int currentSongIndex = 0;
char fileName [20][20];
unsigned long fileSize[20];
long bytesToPlay;
int playbackSpeed = 1; //1x speed by default
int seekBackward = 0; //play forward by default
int play = 1; //wav plays by default
int stop = 0; //deal with the stop state in main instead of ISR

int isWav(char *filename) {
	if (strlen(filename) < 5) {
		return 0; //wav files are at least 5 characters long (ex: a.wav)
	}
	//look for last occurrence of '.' character in filename to get the file extension
	char* fileExtension = strrchr(filename, '.');

	//strcasecmp returns 0 if fileExtension equals ".WAV"; need to invert
	return !strcasecmp(fileExtension, ".WAV");
}

void updateSongIndex() {
	FRESULT res;
	FILINFO Finfo;

	f_opendir(&Dir, 0);
	for (;;) {
		res = f_readdir(&Dir, &Finfo);
		if ((res != FR_OK) || !Finfo.fname[0])
			break; // stop looking for songs when there's no files left in the directory
		char* name = &(Finfo.fname[0]);
		if (isWav(name)) {
			strcpy(fileName[numSongs], name);
			fileSize[numSongs] = Finfo.fsize;
			xprintf("%9lu  %s \n", fileSize[numSongs], fileName[numSongs]);
			numSongs++;
		}
	}
}

void updateLCD(int currentSongIndex) {
	lcd = fopen("/dev/lcd_display", "w");
	fprintf(lcd, "Track %d of %d: \n", currentSongIndex + 1, numSongs); //track# starts at 1
	fprintf(lcd, "%s\n", fileName[currentSongIndex]);
}

void readToAndPlayFromBufferOnce() {
	//play_l and play_r with len > 100, so we can queue stuff into FIFO while f_reading
	f_read(&File1, Buffer, bufferSize*2, &s2);

	//wait until output FIFOs are 75% empty before adding data to it from Buffer
	while (alt_up_audio_write_interrupt_pending(audio_dev) == 0) {}
	int i;
	for (i = 0; i < bufferSize; i+=2*playbackSpeed) {
		// it was found that this order produced the correct left / right outputs (tested LRSTER.wav)
		alt_up_audio_write_fifo_head (audio_dev, (Buffer[i]), ALT_UP_AUDIO_RIGHT);
		alt_up_audio_write_fifo_head (audio_dev, (Buffer[i+1]), ALT_UP_AUDIO_LEFT);
	}
	bytesToPlay -= bufferSize * 2;
}

static void button_ISR(void* context, alt_u32 id){
	//need to de-bounce buttons since one press could result in 2+ interrupts
	usleep(25000); //prevent button press from being interpreted as 2+ interrupts

	int buttonPressed = IORD(BUTTON_PIO_BASE, 0) & 0xF;
	buttonPressed = buttonPressed ^ 0xF; //inverts LSB since buttons are active low; easier to work with
	int isRisingEdge = buttonPressed > 0; //rising edge when we see button currently being pressed
	xprintf("edge %d\n", isRisingEdge);

	if (isRisingEdge) {
		switch (buttonPressed) {
			case 1: // Search Forward button was pressed
				if (play == 0) {
					currentSongIndex = (currentSongIndex + 1) % numSongs;
					f_open(&File1, fileName[currentSongIndex], 1);
					bytesToPlay = fileSize[currentSongIndex];
					updateLCD(currentSongIndex);
					alt_up_audio_reset_audio_core(audio_dev);
				}
				else {
					playbackSpeed = 2;
				}
				break;

			case 2: // Play/Pause button was pressed
				play = play ^ 1; //invert the play flag by XOR with 1
				break;

			case 4: // Stop button was pressed
				play = 0;
				stop = 1;
				break;

			case 8: // Search Backward button was pressed
				if (play) {
					seekBackward = 1; //seek backward state dealt with in main()
				}
				else {
					currentSongIndex--;
					if (currentSongIndex < 0) {
						currentSongIndex = numSongs - 1;
					}
					f_open(&File1, fileName[currentSongIndex], 1);
					bytesToPlay = fileSize[currentSongIndex];
					updateLCD(currentSongIndex);
					alt_up_audio_reset_audio_core(audio_dev);
				}
				break;

			default:
				xprintf("buttonPressed %d\n", buttonPressed);
				break;
		}

		usleep(100000); //prevent button press from being interpreted as 2+ interrupts
	}
	else {
		// if we have falling edge, read edge capture register to check which button was released
		switch (IORD(BUTTON_PIO_BASE,3)) {
			case 1: // Seek Forward button was pressed
				playbackSpeed = 1; //resume normal speed
				break;
			case 8: // Seek Backward button was pressed
				seekBackward = 0; //resume normal playing direction
				break;
			default:
				xprintf("buttonReleased %d\n", IORD(BUTTON_PIO_BASE,3));
				break;
		}
	}

	IOWR(BUTTON_PIO_BASE, 3, 0xF); //clear button interrupt
}

int main(void)
{
	FATFS Fatfs;  //(only one) File system object for SD card logical drive

	//enable button interrupts
	alt_irq_register(BUTTON_PIO_IRQ, (void *)0, button_ISR); //register stimulus with ISR
	IOWR(BUTTON_PIO_BASE, 2, 0xF); //enable interrupts for all 4 buttons

	// open the Audio port
	audio_dev = alt_up_audio_open_dev ("/dev/Audio");
	if ( audio_dev == NULL)
		xprintf ("Error: could not open audio device \n");
	else
		xprintf ("Opened audio device \n");

	//mount drive (fi 0)
	f_mount(0, &Fatfs);

	//initialize fileName and fileSize arrays using SD card data
	updateSongIndex();

	//open file (fo 1 ______.wav)
	while (currentSongIndex < numSongs) {
		f_open(&File1, fileName[currentSongIndex], 1);
		bytesToPlay = fileSize[currentSongIndex];

		// write track # and file name to LCD
		updateLCD(currentSongIndex);

		//enable write interrupt
		IOWR(AUDIO_BASE, 0, 2);
		alt_up_audio_enable_write_interrupt(audio_dev);

		while (bytesToPlay > 0) {
			if (bytesToPlay < bufferSize) {
				bufferSize = bytesToPlay; //don't try to read more data after reading last byte of the .wav file
			}

			readToAndPlayFromBufferOnce();

			//rewind music; short section of track is played in forwards direction after each reverse step
			while(seekBackward) {
				fileOffset = fileSize[currentSongIndex] - bytesToPlay;
				fileOffset -= 2*(2*bufferSize)*1000; //rewind step
				if (fileOffset < 0) {
					fileOffset = 0; //.wav file begins when fileOffset = 0, don't try to access memory before that
					seekBackward = 0; //stop trying to seek if we already reached beginning
				}
				bytesToPlay = fileSize[currentSongIndex] - fileOffset;
				f_lseek(&File1, fileOffset); //the next time f_read is called, it's at an earlier section of the music

				//play short section of track in the forwards direction before next reverse step
				int p = 0;
				for (p = 0; p < 1000; p++) {
					readToAndPlayFromBufferOnce();
				}
			}

			//pause music; at the same time check if the stop button is pressed and handle the logic appropriately
			while (play == 0) {
				//polling for stop flag
				if (stop) {
					//go back to the beginning of the song
					f_open(&File1, fileName[currentSongIndex], 1);
					bytesToPlay = fileSize[currentSongIndex];
					stop = 0; //no need to go back into this conditional
				}
			}
		}

		//disabling interrupts after file is done playing
		IOWR(AUDIO_BASE, 0, 0);
		alt_up_audio_disable_write_interrupt(audio_dev);
		alt_up_audio_reset_audio_core(audio_dev);  // clear FIFO's for both channels

		currentSongIndex++;
	}
	return 0;
}
