// Modified from Zynq Book interrupt_counter_tut_2B.c
#include "xparameters.h"
#include "xgpio.h"
#include "xscugic.h"
#include "xil_exception.h"
#include "xil_printf.h"

#include "xil_mmu.h" // For Xil_SetTlbAttributes();

// Parameter definitions
#define INTC_DEVICE_ID 		XPAR_PS7_SCUGIC_0_DEVICE_ID
#define BTNS_DEVICE_ID		XPAR_AXI_GPIO_0_DEVICE_ID
#define INTC_GPIO_INTERRUPT_ID XPAR_FABRIC_AXI_GPIO_0_IP2INTC_IRPT_INTR

#define BTN_INT 			XGPIO_IR_CH1_MASK

XGpio BTNInst;
XScuGic INTCInst;
static int btn_value;

// Define button values
#define BTN_CENTER 1
#define BTN_DOWN 2
#define BTN_LEFT 4
#define BTN_RIGHT 8
#define BTN_UP 16

#define NUM_STRIPES 8
#define VGA_WIDTH 1280
#define VGA_HEIGHT 1024
#define NUM_BYTES_BUFFER 5242880

#define DIGIT_WIDTH 47
#define DIGIT_HEIGHT 71
#define CIRCLE_WIDTH 155
#define RANKING_WIDTH 660
#define RANKING_HEIGHT 700

#define PIXEL_BYTES 4
struct pixel {
	char red;
	char green;
	char blue;
	char alpha;
};

// Note: Next buffer + 0x7E9004
int *image_output_pointer = (int *)0x02000000;
int *image_buffer_pointer = (int *)0x02FD2008;

// Sprites
extern uint8_t menu[];
extern uint8_t bg[];
extern uint8_t circle[];
extern uint8_t circleOverlay[];
extern uint8_t ranking[];
extern uint8_t num0[];
extern uint8_t num1[];
extern uint8_t num2[];
extern uint8_t num3[];
extern uint8_t num4[];
extern uint8_t num5[];
extern uint8_t num6[];
extern uint8_t num7[];
extern uint8_t num8[];
extern uint8_t num9[];

int *imageMenu = (int *)menu;
int *imageBg = (int *)bg;
int *imageCircle = (int *)circle;
int *imageCircleOverlay = (int *)circleOverlay;
int *imageRanking = (int *)ranking;
int *imageNum[10] = {(int *)num0, (int *)num1, (int *)num2, (int *)num3, (int *)num4, (int *)num5, (int *)num6, (int *)num7, (int *)num8, (int *)num9};

// Stripe Colours
int stripeIndex = 0;
#define NUM_COLOURS 3

int colours[NUM_COLOURS][NUM_STRIPES] =
   {{0xFF0000, 0xFF8000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0000FF, 0x8000FF, 0xFF0080},
	{0x61AE47, 0x85F0C7, 0x8EFEBA, 0xA45F99, 0x6AB9B9, 0xF2543D, 0x28BCFF, 0xD8FD1B},
	{0xCC54A0, 0xEBC73A, 0x3CF442, 0xBD179A, 0xB4E0A4, 0x28C598, 0x186A50, 0x41D51B}};

//----------------------------------------------------
// PROTOTYPE FUNCTIONS
//----------------------------------------------------
static void BTN_Intr_Handler(void *baseaddr_p);
static int InterruptSystemSetup(XScuGic *XScuGicInstancePtr);
static int IntcInitFunction(u16 DeviceId, XGpio *GpioInstancePtr);

void SetPixel(int *pixelAddr, int colour) {
	*pixelAddr = colour;
}

void FillFirstRow(int colour) {
	int *temp_pointer = image_buffer_pointer;
	for (int position = 0; position < VGA_WIDTH; position++) {
		SetPixel(temp_pointer, colour);
		temp_pointer++;
	}
}

void CopyFirstRow() {
	int *temp_pointer_copy = image_buffer_pointer;
	int *temp_pointer_paste = image_buffer_pointer += VGA_WIDTH;
	for (int row = 0; row < VGA_HEIGHT; row++) {
		memcpy(temp_pointer_paste, temp_pointer_copy, VGA_WIDTH * PIXEL_BYTES);
		temp_pointer_paste += VGA_WIDTH;
	}
}

void ChangeStripeColours(int colours[NUM_COLOURS]) {
	int *temp_pointer = image_buffer_pointer;
	for (int stripeIndex = 0; stripeIndex < NUM_STRIPES; stripeIndex++) {
		for (int pixelIndex = 0; pixelIndex < VGA_WIDTH / NUM_STRIPES; pixelIndex++) {
			SetPixel(temp_pointer, colours[stripeIndex]);
			temp_pointer++;
		}
	}

	CopyFirstRow();
}

void RotateDisplayHorizontal(int numPixels) {
	if (numPixels == 0)
		return;

	int *temp_left = image_buffer_pointer;
	if (numPixels > 0) { // Rotate Right
		int blockSize = VGA_WIDTH - numPixels;
		int *temp_right = temp_left + blockSize;

		// Loop through every row
		for (int row = 0; row < VGA_HEIGHT; row++) {
			int tempPixels[numPixels];
			memcpy(tempPixels, temp_right, numPixels * PIXEL_BYTES);

			temp_right = temp_left + numPixels;
			memmove(temp_right, temp_left, blockSize * PIXEL_BYTES);

			memcpy(temp_left, tempPixels, numPixels * PIXEL_BYTES);

			temp_left += VGA_WIDTH;
			temp_right = temp_left + blockSize;
		}
	} else {
		numPixels *= -1;
		int blockSize = VGA_WIDTH - numPixels;
		int *temp_right = temp_left + numPixels;

		for (int row = 0; row < VGA_HEIGHT; row++) {
			int tempPixels[numPixels];
			memcpy(tempPixels, temp_left, numPixels * PIXEL_BYTES);

			memmove(temp_left, temp_right, blockSize * PIXEL_BYTES);

			temp_right = temp_left + blockSize;
			memcpy(temp_right, tempPixels, numPixels * PIXEL_BYTES);

			temp_left += VGA_WIDTH;
			temp_right = temp_left + numPixels;
		}
	}
	memcpy(image_output_pointer, image_buffer_pointer, NUM_BYTES_BUFFER);
}

void FillScreen(int colour) {
	FillFirstRow(colour);
	CopyFirstRow();

	memcpy(image_output_pointer, image_buffer_pointer, NUM_BYTES_BUFFER);
}

// Draws a pixel on top of another considering transparency
// Referenced https://gist.github.com/XProger/96253e93baccfbf338de
void PixelAlpha(int *under, int *over) {
	int alpha = (*over & 0xFF000000) >> 24;

	if (alpha == 0)
		return;

	if (alpha == 0xFF) {
		*under = *over;
		return;
	}

	unsigned int rb = *under & 0x00FF00FF;
	unsigned int g = *under & 0x0000FF00;

	rb += ((*over & 0x00FF00FF) - rb) * alpha >> 8;
	g += ((*over & 0x0000FF00) - g) * alpha >> 8;

	*under = (rb & 0x00FF00FF) | (g & 0x0000FF00);
}

// Draws a sprite on top of the buffer frame
void DrawSprite(int *sprite, int width, int height, int posX, int posY) {
	int *temp_pointer = (int *)image_buffer_pointer;
	int *sprite_pointer = (int *)sprite;
	temp_pointer += posX + posY * VGA_WIDTH;

	for (int currX = posX; currX < posX + height; currX++) {
		for (int currY = posY; currY < posY + width; currY++) {
			PixelAlpha(temp_pointer, sprite_pointer);

			temp_pointer++;
			sprite_pointer++;
		}
		temp_pointer += VGA_WIDTH - width;
	}
}

void DrawInt(unsigned int num, int length, int posX, int posY) {
	posX += DIGIT_WIDTH * (length - 1);

	for (int digitPos = 1; digitPos <= length; digitPos++) {
		int digit = num % 10;
		DrawSprite(imageNum[digit], DIGIT_WIDTH, DIGIT_HEIGHT, posX, posY);

		num /= 10;
		posX -= DIGIT_WIDTH;
	}
}

unsigned int score = 0;

#define SCREEN_MENU 0
#define SCREEN_GAME 1
#define SCREEN_STAT 2
int screen = SCREEN_MENU;

void DrawMenu() {
	screen = SCREEN_MENU;

	memcpy(image_buffer_pointer, imageMenu, NUM_BYTES_BUFFER);
	memcpy(image_output_pointer, image_buffer_pointer, NUM_BYTES_BUFFER);
}

void DrawGame() {
	screen = SCREEN_GAME;

	memcpy(image_buffer_pointer, imageBg, NUM_BYTES_BUFFER);
	DrawInt(score, 7, 951, 0);
	DrawSprite(imageCircle, CIRCLE_WIDTH, CIRCLE_WIDTH, 400, 500);
	DrawSprite(imageCircleOverlay, CIRCLE_WIDTH, CIRCLE_WIDTH, 400, 500);
	DrawSprite(imageCircle, CIRCLE_WIDTH, CIRCLE_WIDTH, 700, 250);
	DrawSprite(imageCircleOverlay, CIRCLE_WIDTH, CIRCLE_WIDTH, 700, 250);
	memcpy(image_output_pointer, image_buffer_pointer, NUM_BYTES_BUFFER);
}

void DrawStats() {
	screen = SCREEN_STAT;

	memcpy(image_buffer_pointer, imageBg, NUM_BYTES_BUFFER);
	DrawSprite(imageRanking, RANKING_WIDTH, RANKING_HEIGHT, 0, 200);
	DrawInt(score, 7, 150, 210);
	DrawInt(score, 3, 20, 635);
	DrawInt(score, 3, 310, 635);
	memcpy(image_output_pointer, image_buffer_pointer, NUM_BYTES_BUFFER);
}

//----------------------------------------------------
// INTERRUPT HANDLER FUNCTIONS
// - called by the timer, button interrupt
// - modified to change VGA output
//----------------------------------------------------
void BTN_Intr_Handler(void *InstancePtr)
{
	// Disable GPIO interrupts
	XGpio_InterruptDisable(&BTNInst, BTN_INT);

	// Ignore additional button presses
	if ((XGpio_InterruptGetStatus(&BTNInst) & BTN_INT) != BTN_INT) {
		return;
	}

	btn_value = XGpio_DiscreteRead(&BTNInst, 1);

	// Change display when buttons are pressed
	if (btn_value == BTN_CENTER) {
		score = 0;
		DrawMenu();
	} else if (btn_value == BTN_LEFT) {
		DrawGame();
	} else if (btn_value == BTN_RIGHT) {
		DrawStats();
	} else if (btn_value == BTN_UP) {
		score += 369;
		if (screen == SCREEN_GAME)
			DrawGame();
		else if (screen == SCREEN_STAT)
			DrawStats();

	} else if (btn_value == BTN_DOWN) {
		score -= 144;
		if (screen == SCREEN_GAME)
			DrawGame();
		else if (screen == SCREEN_STAT)
			DrawStats();
	}

    (void)XGpio_InterruptClear(&BTNInst, BTN_INT);

    // Enable GPIO interrupts
    XGpio_InterruptEnable(&BTNInst, BTN_INT);
}

//----------------------------------------------------
// MAIN FUNCTION
//----------------------------------------------------
int main (void)
{
	int status;
	//----------------------------------------------------
	// INITIALIZE THE PERIPHERALS & SET DIRECTIONS OF GPIO
	//----------------------------------------------------
	// Initialise Push Buttons
	status = XGpio_Initialize(&BTNInst, BTNS_DEVICE_ID);
	if(status != XST_SUCCESS) return XST_FAILURE;
	// Set all buttons direction to inputs
	XGpio_SetDataDirection(&BTNInst, 1, 0xFF);

	// Initialize interrupt controller
	status = IntcInitFunction(INTC_DEVICE_ID, &BTNInst);
	if(status != XST_SUCCESS) return XST_FAILURE;



  	// Set Output Buffer as Non-Cacheable
  	for (int i = 0; i <= 512; i++) {
  		Xil_SetTlbAttributes((INTPTR)(image_output_pointer + i * 2048), NORM_NONCACHE);
  	}

  	DrawMenu();



	while(1);

	return 0;
}

//----------------------------------------------------
// INITIAL SETUP FUNCTIONS
//----------------------------------------------------

int InterruptSystemSetup(XScuGic *XScuGicInstancePtr)
{
	// Enable interrupt
	XGpio_InterruptEnable(&BTNInst, BTN_INT);
	XGpio_InterruptGlobalEnable(&BTNInst);

	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
			 	 	 	 	 	 (Xil_ExceptionHandler)XScuGic_InterruptHandler,
			 	 	 	 	 	 XScuGicInstancePtr);
	Xil_ExceptionEnable();


	return XST_SUCCESS;

}

int IntcInitFunction(u16 DeviceId, XGpio *GpioInstancePtr)
{
	XScuGic_Config *IntcConfig;
	int status;

	// Interrupt controller initialisation
	IntcConfig = XScuGic_LookupConfig(DeviceId);
	status = XScuGic_CfgInitialize(&INTCInst, IntcConfig, IntcConfig->CpuBaseAddress);
	if(status != XST_SUCCESS) return XST_FAILURE;

	// Call to interrupt setup
	status = InterruptSystemSetup(&INTCInst);
	if(status != XST_SUCCESS) return XST_FAILURE;

	// Connect GPIO interrupt to handler
	status = XScuGic_Connect(&INTCInst,
					  	  	 INTC_GPIO_INTERRUPT_ID,
					  	  	 (Xil_ExceptionHandler)BTN_Intr_Handler,
					  	  	 (void *)GpioInstancePtr);
	if(status != XST_SUCCESS) return XST_FAILURE;

	// Enable GPIO interrupts interrupt
	XGpio_InterruptEnable(GpioInstancePtr, 1);
	XGpio_InterruptGlobalEnable(GpioInstancePtr);

	// Enable GPIO and timer interrupts in the controller
	XScuGic_Enable(&INTCInst, INTC_GPIO_INTERRUPT_ID);

	return XST_SUCCESS;
}
