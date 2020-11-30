#include "vga.h"

#define VTOTAL	52							    /* Total bytes to send through SPI */
__align(4) uint8_t fb[VID_VSIZE][VID_HSIZE+2];	/* Frame buffer 200x(50+2)x8*/
static volatile uint16_t vline = 0;				/* The current line being drawn */
static volatile uint32_t vflag = 0;				/* When 1, the SPI DMA request can draw on the screen */
static volatile uint32_t vdraw = 0;				/* Used to increment vline every 3 drawn lines */ 

extern const u8 vga_word[];

void timer_config(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	NVIC_InitTypeDef nvic;
	TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
	TIM_OCInitTypeDef TIM_OCInitStructure;
	uint32_t TimerPeriod = 0;
	uint16_t Channel1Pulse = 0, Channel2Pulse = 0, Channel3Pulse = 0;
	
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_1 | GPIO_Pin_8;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
	GPIO_Init(GPIOA, &GPIO_InitStructure);
	
	/*
		SVGA 800x600 @ 56 Hz
		Vertical refresh	35.15625 kHz
		Pixel freq.			36.0 MHz
		
		1 system tick @ 72Mhz = 0.0139 us
	*/
	
	/*
		Horizontal timing
		-----------------
		
		Timer 1 period = 35156 Hz
		
		Timer 1 channel 1 generates a pulse for HSYNC each 28.4 us.
		28.4 us	= Visible area + Front porch + Sync pulse + Back porch.
		HSYNC is 2 us long, so the math to do is:
		2us / 0.0139us = 144 system ticks.
		
		Timer 1 channel 2 generates a pulse equal to HSYNC + back porch.
		This interrupt will fire the DMA request to draw on the screen if vflag == 1.
		Since firing the DMA takes more or less 800ns, we'll add some extra time.
		The math for HSYNC + back porch is:
		(2us + 3.55us - dma) / 0.0139 = +-350 system ticks
	
		Horizontal timing info
		----------------------

						Dots	us
		--------------------------------------------		
		Visible area	800		22.222222222222
		Front porch		24		0.66666666666667
		Sync pulse		72		2
		Back porch		128		3.5555555555556
		Whole line		1024	28.444444444444
	
	*/

	TimerPeriod = 2048;
	Channel1Pulse = 144;		/* HSYNC */
	Channel2Pulse = 352; 		/* HSYNC + BACK PORCH */
	
	TIM_TimeBaseStructure.TIM_Prescaler = 0;
	TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
	TIM_TimeBaseStructure.TIM_Period = TimerPeriod;
	TIM_TimeBaseStructure.TIM_ClockDivision = 0;
	TIM_TimeBaseStructure.TIM_RepetitionCounter = 0;
	TIM_TimeBaseInit(TIM1, &TIM_TimeBaseStructure);

	TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM2;
	TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
	TIM_OCInitStructure.TIM_OutputNState = TIM_OutputNState_Enable;
	TIM_OCInitStructure.TIM_Pulse = Channel1Pulse;
	TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_Low;
	TIM_OCInitStructure.TIM_OCNPolarity = TIM_OCNPolarity_High;
	TIM_OCInitStructure.TIM_OCIdleState = TIM_OCIdleState_Reset;
	TIM_OCInitStructure.TIM_OCNIdleState = TIM_OCIdleState_Set;

	TIM_OC1Init(TIM1, &TIM_OCInitStructure);
	
	TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_Inactive;
	TIM_OCInitStructure.TIM_Pulse = Channel2Pulse;
	TIM_OC2Init(TIM1, &TIM_OCInitStructure);

	/* TIM1 counter enable and output enable */
	TIM_CtrlPWMOutputs(TIM1, ENABLE);

	/* Select TIM1 as Master */
	TIM_SelectMasterSlaveMode(TIM1, TIM_MasterSlaveMode_Enable);
	TIM_SelectOutputTrigger(TIM1, TIM_TRGOSource_Update);
	
	/*
		Vertical timing
		---------------
		
		Polarity of vertical sync pulse is positive.

						Lines
		------------------------------
		Visible area	600
		Front porch		1
		Sync pulse		2
		Back porch		22
		Whole frame		625
		
	*/

	/* VSYNC (TIM2_CH2) and VSYNC_BACKPORCH (TIM2_CH3) */
	/* Channel 2 and 3 Configuration in PWM mode */
	TIM_SelectSlaveMode(TIM2, TIM_SlaveMode_Gated);
	TIM_SelectInputTrigger(TIM2, TIM_TS_ITR0);              // Set TIM1 as trigger
	
	TimerPeriod = 625;		/* Vertical lines */
	Channel2Pulse = 2;		/* Sync pulse */
	Channel3Pulse = 24;		/* Sync pulse + Back porch */
	TIM_TimeBaseStructure.TIM_Prescaler = 0;
	TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
	TIM_TimeBaseStructure.TIM_Period = TimerPeriod;
	TIM_TimeBaseStructure.TIM_ClockDivision = 0;
	TIM_TimeBaseStructure.TIM_RepetitionCounter = 0;

	TIM_TimeBaseInit(TIM2, &TIM_TimeBaseStructure);

	TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM2;
	TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
	TIM_OCInitStructure.TIM_OutputNState = TIM_OutputNState_Enable;
	TIM_OCInitStructure.TIM_Pulse = Channel2Pulse;
	TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_Low;
	TIM_OCInitStructure.TIM_OCNPolarity = TIM_OCNPolarity_High;
	TIM_OCInitStructure.TIM_OCIdleState = TIM_OCIdleState_Reset;
	TIM_OCInitStructure.TIM_OCNIdleState = TIM_OCIdleState_Set;
	TIM_OC2Init(TIM2, &TIM_OCInitStructure);
	
	TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_Inactive;
	TIM_OCInitStructure.TIM_Pulse = Channel3Pulse;
	TIM_OC3Init(TIM2, &TIM_OCInitStructure);

	/*	TIM2 counter enable and output enable */
	TIM_CtrlPWMOutputs(TIM2, ENABLE);

	/* Interrupt TIM2 */
	nvic.NVIC_IRQChannel = TIM2_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;

	NVIC_Init(&nvic);
	TIM_ITConfig(TIM2, TIM_IT_CC3, ENABLE);

	/* Interrupt TIM1 */
	nvic.NVIC_IRQChannel = TIM1_CC_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;

	NVIC_Init(&nvic);
	TIM_ITConfig(TIM1, TIM_IT_CC2, ENABLE);
	
	TIM_Cmd(TIM2, ENABLE);
	TIM_Cmd(TIM1, ENABLE);
}

void spi_config(void)
{
	NVIC_InitTypeDef nvic;
	SPI_InitTypeDef SPI_InitStructure;
	DMA_InitTypeDef	DMA_InitStructure;
	GPIO_InitTypeDef GPIO_InitStructure;
	
	GPIO_InitStructure.GPIO_Pin =  GPIO_Pin_7;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	SPI_Cmd(SPI1, DISABLE);
	DMA_DeInit(DMA1_Channel3);

	DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&SPI1->DR;
	DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t) &fb[0][0];
	DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralDST;
	DMA_InitStructure.DMA_BufferSize = VTOTAL;
	DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
	DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
	DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
	DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
	DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;
	DMA_InitStructure.DMA_Priority = DMA_Priority_Low;
	DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
	DMA_Init(DMA1_Channel3, &DMA_InitStructure);

	SPI_InitStructure.SPI_Direction = SPI_Direction_1Line_Tx;
	SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
	SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
	SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;
	SPI_InitStructure.SPI_CPHA = SPI_CPHA_2Edge;
	SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
	SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_4;
	SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
	SPI_InitStructure.SPI_CRCPolynomial = 7;
	SPI_Init(SPI1, &SPI_InitStructure);
	
	SPI_CalculateCRC(SPI1, DISABLE);
	SPI_Cmd(SPI1, ENABLE);
	
	SPI1->CR2 |= SPI_I2S_DMAReq_Tx;
	
	nvic.NVIC_IRQChannel = DMA1_Channel3_IRQn;
	nvic.NVIC_IRQChannelPreemptionPriority = 0;
	nvic.NVIC_IRQChannelSubPriority = 0;
	nvic.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&nvic);
	
	DMA1_Channel3->CCR &= ~1;
	DMA1_Channel3->CNDTR = VTOTAL;
	DMA1_Channel3->CMAR = (uint32_t) &fb[0][0];
	
	DMA_ITConfig(DMA1_Channel3, DMA_IT_TC, ENABLE);
}

//*****************************************************************************
//	This irq is generated at the end of the horizontal back porch.
//	Test if inside a valid vertical start frame (vflag variable), 
//	and start the DMA to output a single frame buffer line through the SPI device.
//*****************************************************************************
__irq void TIM1_CC_IRQHandler(void)
{
	if (vflag) {
		DMA1_Channel3->CCR = 0x93;
	}
	TIM1->SR = 0xFFFB; //~TIM_IT_CC2;
}

//*****************************************************************************
//	This irq is generated at the end of the vertical back porch.
//	Sets the 'vflag' variable to 1 (valid vertical frame).
//*****************************************************************************
__irq void TIM2_IRQHandler(void)
{
	vflag = 1;
	TIM2->SR = 0xFFF7; //~TIM_IT_CC3;
}

//*****************************************************************************
//	This interrupt is generated at the end of every line.
//	It will increment the line number and set the corresponding line pointer
//	in the DMA register.
//*****************************************************************************
__irq void DMA1_Channel3_IRQHandler(void)
{	
	DMA1->IFCR = DMA1_IT_TC3;
	DMA1_Channel3->CCR = 0x92;
	DMA1_Channel3->CNDTR = VTOTAL;
	
	vdraw++;
	
	if (vdraw == 3) {
		vdraw = 0;

		vline++;
		
		if (vline == VID_VSIZE) {
			vdraw = vline = vflag = 0;
			DMA1_Channel3->CMAR = (uint32_t) &fb[0][0];
		} else {
			DMA1_Channel3->CMAR += VTOTAL;
		}
	}
}

void vga_clear_screen(void)
{
	uint16_t x, y;

	for (y = 0; y < VID_VSIZE; y++) {
		for (x = 0; x < VTOTAL; x++) {
			fb[y][x] = 0;
		}
	}
}

void vga_init(void)
{
	spi_config();
	timer_config();
	vga_clear_screen();
}



//======================================================== JUST A LINE =================================================================



//*****************************************************************************
//	Bit Block Transfer funcion. This function uses the STM32 Bit-Banding mode
//	to simplify the complex BitBlt functionality.
//
//	From Cortex STM32F10x Reference Manual (RM0008):
//	A mapping formula shows how to reference each word in the alias region to a 
//	corresponding bit in the bit-band region. The mapping formula is:
//	bit_word_addr = bit_band_base + (byte_offset x 32) + (bit_number נ4)
//	where:
//	bit_word_addr is the address of the word in the alias memory region that 
//	maps to the targeted bit.
//	bit_band_base is the starting address of the alias region
//	byte_offset is the number of the byte in the bit-band region that contains 
//	the targeted bit bit_number is the bit position (0-7) of the targeted bit.
//	Example:
//	The following example shows how to map bit 2 of the byte located at SRAM 
//	address 0x20000300 in the alias region:
//	0x22006008 = 0x22000000 + (0x300*32) + (2*4).
//	Writing to address 0x22006008 has the same effect as a read-modify-write 
//	operation on bit 2 of the byte at SRAM address 0x20000300.
//	Reading address 0x22006008 returns the value (0x01 or 0x00) of bit 2 of 
//	the byte at SRAM address 0x20000300 (0x01: bit set; 0x00: bit reset).
//
//	For further reference see the Cortex M3 Technical Reference Manual
//
//	Parameters:
//
//		prc			Clipping rectangle. All X/Y coordinates are inside "prc"
//					If "prc" is NULL, the coordinates will be the entire display
//					area
//		x			Bitmap X start position
//		y			Bitmap Y start position
//		w			Bitmap width, in pixels
//		y			Bitmap height, in pixels
//		bm			Pointer to te bitmap start position
//		rop			Raster operation. See GDI_ROP_xxx defines
//
//	return			none
//*****************************************************************************
void vga_bitblt(PGDI_RECT prc, int16_t x, int16_t y, int16_t w, int16_t h, pu8 bm, uint16_t rop)
{
    uint16_t i, xz, xb, xt;
    uint32_t wb;				// Width in bytes
    uint32_t r;					// Start X position in bits (relative to x)
    uint32_t k;				
    uint32_t d;
    uint32_t offs;
    uint8_t  c;
    pu8	     fbPtr;				// Pointer to the Frame Buffer Bit-Band area
    pu8	     fbBak;
    u8	     fb1;
    uint32_t fb2;
    uint32_t rp;
    pu8	     bmPtr;				// Pointer to the bitmap bits

    // Calculate clipping region
	if (prc != NULL) {
		x = prc->x + x;
		y = prc->y + y;
	}

    // Get total bitmap width in bytes
	wb = (uint32_t) w >> 3;
    // If the width is less than 1 byte, set it to 1 byte
	if ((wb << 3) < (uint32_t) w) ++wb;

    // Get starting bit inside the first byte
	d = (uint32_t) x >> 3;
	r  = ((uint32_t) x - (d << 3));

    // Clip X
	if (prc == NULL) {
		if ((x + w) >= VID_PIXELS_X ) {
			xt =  VID_PIXELS_X - x;
		} else {
			xt = w;
		}
	} else {
		if ((x + w) >= (x + prc->w)) {
			xt = prc->w - x;
		} else {
			xt = w;
		}
	}

    // Draw bits
	for (i = 0; i < h; i++) {
        // Clip Y
		if ((i + y) > (VID_VSIZE - 1)) return;

        // Get offset to frame buffer in bit-banding mode
		offs = (((u32) x >> 3)) + ((u32) (y + i)  * VID_HSIZE_R);
		k = (u32) (&fb - 0x20000000);
		k += offs;
		fbPtr = (pu8) (0x22000000 + (k * 32) + ((7 - r) * 4));
		fbBak = (pu8) (0x22000000 + (k * 32) + 28);

        // Get offset to bitmap bits
		bmPtr = bm + ((u32) i * wb);
		xz = w;

		xb = 0;
		for (xz = 0; xz < xt; xz++) {
			fb1 = ((u32) fbPtr) & 0x000000E0;
			if (xb++ == 0) {
				c = *bmPtr;
				++bmPtr;
			}
			xb &= 0x07;
			(c & 0x80) ? (rp = 1) : (rp = 0);
			switch(rop) {
				case GDI_ROP_COPY:	*fbPtr = rp;		break;
				case GDI_ROP_XOR:	*fbPtr ^= rp;		break;
				case GDI_ROP_AND:	*fbPtr &= rp;		break;
				case GDI_ROP_OR:	*fbPtr |= rp;		break;
			}
			fbPtr -= 4;
			fb2 = ((u32) fbPtr) & 0x000000E0;
			if (fb1 != fb2) {
				fbPtr = fbBak + 32;
				fbBak = fbPtr;
			}
			c <<= 1;
		}
	}
}

//*****************************************************************************
//	Draw text in X/Y position using system font.
//
//	parameters:
//		x			X start position
//		y			Y start position
//		ptext		Pointer to text
//		rop			Raster operation. See GDI_ROP_xxx defines
//
//	return			none
//*****************************************************************************
void vga_draw_text(int16_t x, int16_t y, pu8 ptext, u16 rop)
{

    u16	l, i, pos, xp;
    u8	c;
    pu8	ptx;

	l = strLen(ptext);
	xp = x;
	for (i = 0; i < l; i++) {
		c = *(ptext++);
		if (c >= GDI_SYSFONT_OFFSET) {
			pos = (u16) (c - GDI_SYSFONT_OFFSET) * GDI_SYSFONT_BYTEWIDTH * GDI_SYSFONT_HEIGHT;
			ptx = ((pu8) vga_word) + pos;
			vga_bitblt(NULL, xp, y, GDI_SYSFONT_WIDTH, GDI_SYSFONT_HEIGHT, ptx, rop);
			xp += GDI_SYSFONT_WIDTH;
			if (xp >= VID_PIXELS_X) return;
		}
	}
}


// Specify the number of the letters to draw
void vga_draw_nwords(int16_t x, int16_t y, pu8 ptext, u16 rop, u16 l)
{

    u16	i, pos, xp;
    u8	c;
    pu8	ptx;

	xp = x;
	for (i = 0; i < l; i++) {
		c = *(ptext++);
		if (c >= GDI_SYSFONT_OFFSET) {
			pos = (u16) (c - GDI_SYSFONT_OFFSET) * GDI_SYSFONT_BYTEWIDTH * GDI_SYSFONT_HEIGHT;
			ptx = ((pu8) vga_word) + pos;
			vga_bitblt(NULL, xp, y, GDI_SYSFONT_WIDTH, GDI_SYSFONT_HEIGHT, ptx, rop);
			xp += GDI_SYSFONT_WIDTH;
			if (xp >= VID_PIXELS_X) return;
		}
	}
}
//*****************************************************************************
//	Draw text inside rectangle
//
//	parameters:
//		prc			Pointer to clipping rectangle
//		ptext		Pointer to text
//		style		Text style (see GDI_WINCAPTION_xx defines)
//		rop			Raster operation. See GDI_ROP_xxx defines
//
//	return			none
//*****************************************************************************
void vga_draw_textrec(PGDI_RECT prc, pu8 ptext, uint16_t style, uint16_t rop) 
{
    uint16_t l1, l2, i, pos, xp;
    uint8_t	c;
    pu8	ptx;

    l1 = strLen(ptext);
	l2 = l1 * GDI_SYSFONT_WIDTH;
    
    // Decide where to start painting
	switch(style) {
		case GDI_WINCAPTION_RIGHT:		
            if (l2 < prc->w)
                prc->x += (prc->w - l2);
            break;
		case GDI_WINCAPTION_CENTER:	
            if (l2 < prc->w) 							
                prc->x += ((prc->w - l2) / 2);										
            break;
	}
	xp = 1; //prc->x;
	for (i = 0; i < l1; i++) {
		c = *(ptext++);
		if (c >= GDI_SYSFONT_OFFSET) {
            // Get the position of the letter in vga_word[]
			pos = (uint16_t) (c - GDI_SYSFONT_OFFSET) * GDI_SYSFONT_BYTEWIDTH * GDI_SYSFONT_HEIGHT;
			ptx = ((pu8) vga_word) + pos;
            
			vga_bitblt(prc, xp, 0, GDI_SYSFONT_WIDTH, GDI_SYSFONT_HEIGHT, ptx, rop);
			xp += GDI_SYSFONT_WIDTH;
			if (xp >= ((prc->x + prc->w) - GDI_SYSFONT_WIDTH)) return;
		}
	}
}

//*****************************************************************************
//	Show a point in x/y position using the current graphical mode stored in 
//	grMode variable
//
//	parameters:
//		x			X position
//		y			Y position
//		rop			Raster operation. See GDI_ROP_xxx defines
//
//	return:			none
//*****************************************************************************
void vga_draw_point(PGDI_RECT rc, u16 x, u16 y, u16 rop)
{

    u16	w, r;
    u8	m;

    // Test for point outside display area

	if (x >= VID_PIXELS_X || y >= VID_PIXELS_Y) return;

	w = x >> 3;
	r = x - (w << 3);

    // Prepare mask
	m = (0x80 >> r);

	switch(rop) {
		case GDI_ROP_COPY:		fb[y][w] |= m;
								break;
		case GDI_ROP_XOR:		fb[y][w] ^= m;
								break;
		case GDI_ROP_AND:		fb[y][w] &= m;
								break;
	}
}

//*****************************************************************************
//	Draw line using Bresenham algorithm 
//
//	This function was taken from the book:
//	Interactive Computer Graphics, A top-down approach with OpenGL
//	written by Emeritus Edward Angel
//
//	parameters:
//		prc			Clipping rectangle
//		x1			X start position
//		y1			Y start position
//		x2			X end position
//		y2			Y end position
//		rop			Raster operation. See GDI_ROP_xxx defines
//
//	return			none
//*****************************************************************************
void vga_draw_line(PGDI_RECT prc, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t rop) 
{
    int16_t	dx, dy, i, e;
    int16_t	incx = 1, incy = 1, inc1, inc2;
    int16_t	x, y;

	dx = x2 - x1;
	dy = y2 - y1;

	if(dx < 0) {
        dx = -dx;
        incx = -1;
    }
	if(dy < 0) { 
        dy = -dy;
        incy = -1;
    }

	x=x1;
	y=y1;

	if (dx > dy) {
		vga_draw_point(prc, x, y, rop);
		e = 2*dy - dx;
		inc1 = 2 * ( dy -dx);
		inc2 = 2 * dy;
		for (i = 0; i < dx; i++) {
			if (e >= 0) {
				y += incy;
				e += inc1;
			}
			else {
				e += inc2;
			}
			x += incx;
			vga_draw_point(prc, x, y, rop);
		}
	} else {
		vga_draw_point(prc, x, y, rop);
		e = 2 * dx - dy;
		inc1 = 2 * (dx - dy);
		inc2 = 2 * dx;
		for(i = 0; i < dy; i++) {
			if (e >= 0) {
				x += incx;
				e += inc1;
			} else {
				e += inc2;
			}
			y += incy;
			vga_draw_point(prc, x, y, rop);
		}
	}
}

//*****************************************************************************
//	Draw rectangle
//
//	parameters:
//		x1			X start position
//		y1			Y start position
//		x2			X end position
//		y2			Y end position
//		rop			Raster operation. See GDI_ROP_xxx defines
//
//	return			none
//*****************************************************************************
void vga_draw_rec(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t rop) 
{
	vga_draw_line(NULL, x0, y0, x1, y0, rop);
	vga_draw_line(NULL, x0, y1, x1, y1, rop);
	vga_draw_line(NULL, x0, y0, x0, y1, rop);
	vga_draw_line(NULL, x1, y0, x1, y1, rop);
}



