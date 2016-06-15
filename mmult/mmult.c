/******************************************************************************
*
* Copyright (C) 2009 - 2014 Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* Use of the Software is limited solely to applications:
* (a) running on a Xilinx device, or
* (b) that interact with a Xilinx device through a bus or interconnect.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Except as contained in this notice, the name of the Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Xilinx.
*
******************************************************************************/

/*
 * helloworld.c: simple test application
 *
 * This application configures UART 16550 to baud rate 9600.
 * PS7 UART (Zynq) is not initialized by this application, since
 * bootrom/bsp configures it to baud rate 115200
 *
 * ------------------------------------------------
 * | UART TYPE   BAUD RATE                        |
 * ------------------------------------------------
 *   uartns550   9600
 *   uartlite    Configurable only in HW design
 *   ps7_uart    115200 (configured by bootrom/bsp)
 */

#include <stdio.h>
#include <stdlib.h>
#include "platform.h"
#include "xparameters.h"
#include "xscugic.h"
#include "xscutimer.h"
#include "Xmmult.h"
#include <math.h>

#define NS 1e9
#define US 1e6
#define MS 1e3
#define SS  1

#define timerCal(x) (XPAR_PS7_CORTEXA9_0_CPU_CLK_FREQ_HZ/(2*(x)))
#define timerCounter (int)timerCal(US)
#define timePerInterrupt 2.0*timerCounter/XPAR_PS7_CORTEXA9_0_CPU_CLK_FREQ_HZ

#define L 256
int interrupt_get,InterruptCounter,MaxCounter=1000000;
int mmult_init(XMmult *hestonEuro)
{
	int status;
	XMmult_Config * cfgPtr;
	cfgPtr=XMmult_LookupConfig(XPAR_MMULT_0_DEVICE_ID);
	if(!cfgPtr)
	{
		print("ERROR: Lookup of accelerator configuration failed.\n'r");
		return XST_FAILURE;
	}
	status = XMmult_CfgInitialize(hestonEuro,cfgPtr);
	if(status!=XST_SUCCESS)
	{
		print("ERROR: Could not initialize accelerator.\n\r");
		return XST_FAILURE;
	}
	return status;
}
void hestonEuro_start(void *InstancePtr)
{
	XMmult *pAccelerator = (XMmult *)InstancePtr;
	XMmult_InterruptEnable(pAccelerator,1);
	XMmult_InterruptGlobalEnable(pAccelerator);
	XMmult_Start(pAccelerator);
}

int timer_init(XScuTimer  *ptrInstance)
{
	XScuTimer_Config *cfgPtr;
	int status;
	cfgPtr = XScuTimer_LookupConfig(XPAR_PS7_SCUTIMER_0_DEVICE_ID);
	if (!cfgPtr) {
		print("ERROR: Lookup of timer configuration failed.\n\r");
		return XST_FAILURE;
	}
	status = XScuTimer_CfgInitialize(ptrInstance, cfgPtr,cfgPtr->BaseAddr);
	if (status != XST_SUCCESS) {
		print("ERROR: Could not initialize timer.\n\r");
		return XST_FAILURE;
	}
	// Load the timer with a value that represents one second of real time
		// HINT: The SCU Timer is clocked at half the frequency of the CPU.
	XScuTimer_LoadTimer(ptrInstance, timerCounter);

	// Enable Auto reload mode on the timer.  When it expires, it re-loads
	// the original value automatically.  This means that the timing interval
	// is never skewed by the time taken for the interrupt handler to run
	XScuTimer_EnableAutoReload(ptrInstance);
	// Enable the interrupt *output* in the timer.
	XScuTimer_EnableInterrupt(ptrInstance);
	return status;
}
static void timer_isr(void *CallBackRef)
{
	// The Xilinx drivers automatically pass an instance of
	// the peripheral which generated in the interrupt into this
	// function, using the special parameter called "CallBackRef".
	// We will locally declare an instance of the timer, and assign
	// it to CallBackRef.  You'll see why in a minute.
	XScuTimer *my_Timer_LOCAL = (XScuTimer *) CallBackRef;

	// Here we'll check to see if the timer counter has expired.
	// Technically speaking, this check is not necessary.
	// We know that the timer has expired because that's the
	// reason we're in this handler in the first place!
	// However, this is an example of how a callback reference
	// can be used as a pointer to the instance of the timer
	// that expired.  If we had two timers then we could use the same
	// handler for both, and the "CallBackRef" would always tell us
	// which timer had generated the interrupt.
	if (XScuTimer_IsExpired(my_Timer_LOCAL))
	{
		// Clear the interrupt flag in the timer, so we don't service
		// the same interrupt twice.
		XScuTimer_ClearInterruptStatus(my_Timer_LOCAL);

		// Increment a counter so that we know how many interrupts
		// have been generated.  The counter is a global variable
		InterruptCounter++;
		// Print something to the UART to show that we're in the interrupt handler
		// Check to see if we've had more than the defined number of interrupts
		if (interrupt_get!=0 || InterruptCounter>=MaxCounter)
		{
			// Stop the timer from automatically re-loading, so
			// that we don't get any more interrupts.
			// We couldn't do this if we didn't have the CallBackRef
			interrupt_get=1;
			XScuTimer_DisableAutoReload(my_Timer_LOCAL);
		}
	}
}

static void mmult_isr(void *InstancePtr)
{
	XMmult *pAccelerator = (XMmult *)InstancePtr;
	XMmult_InterruptGlobalDisable(pAccelerator);
	XMmult_InterruptDisable(pAccelerator,0xffffffff);
	XMmult_InterruptClear(pAccelerator,1);
	interrupt_get=1;
}

int setupt_interrupt(XScuGic* scugic,XMmult *pblack,XScuTimer*ptimer)
{
	int result;
	XScuGic_Config *pCfg=XScuGic_LookupConfig(XPAR_SCUGIC_SINGLE_DEVICE_ID);
	if(pCfg==NULL)
	{
		print("Interrupt Configuration Lookup Failed\n\r");
		return XST_FAILURE;
	}
	result=XScuGic_CfgInitialize(scugic,pCfg,pCfg->CpuBaseAddress);
	if(result!=XST_SUCCESS)
		return result;
	result=XScuGic_SelfTest(scugic);
	if(result!=XST_SUCCESS)
	{
		print("selftest failed.\n");
		return result;
	}
	Xil_ExceptionInit();
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,(Xil_ExceptionHandler)XScuGic_InterruptHandler,scugic);

	result=XScuGic_Connect(scugic,XPAR_FABRIC_MMULT_0_INTERRUPT_INTR,(Xil_InterruptHandler)mmult_isr,pblack);
	if(result!=XST_SUCCESS)
	{
		print("ERROR: Could not set up isr.\n\r");
		return result;
	}
	XScuGic_Enable(scugic,XPAR_FABRIC_MMULT_0_INTERRUPT_INTR);
	result = XScuGic_Connect(scugic, XPAR_SCUTIMER_INTR, (Xil_ExceptionHandler)timer_isr, (void *)ptimer);
	if (result != XST_SUCCESS) {
		print("ERROR: Could not set up isr.\n\r");
		return XST_FAILURE;
	}
	// Enable the interrupt *input* on the GIC for the timer's interrupt
	XScuGic_Enable(scugic, XPAR_SCUTIMER_INTR);
	// Enable interrupts in the ARM Processor.
	Xil_ExceptionEnable();
	return XST_SUCCESS;
}
int main()
{

	int i,test=0;
	init_platform();

	XMmult mmulter;
	XScuGic ScuGic;
	XScuTimer timer;
	int status;
    interrupt_get=0;
    InterruptCounter=0;
    status=mmult_init(&mmulter);
    if(status!=XST_SUCCESS)
    {
    	print("hestonEuro peripheral setup failed\n\r");
    	exit(-1);
    }
    status=timer_init(&timer);
	if(status!=XST_SUCCESS)
	{
		print("timer peripheral setup failed\n\r");
		exit(-1);
	}
    status=setupt_interrupt(&ScuGic,&mmulter,&timer);
    if(status!=XST_SUCCESS)
    {
       	print("Interrupt setup failed\n\r");
       	exit(-1);
    }


    if(XMmult_IsReady(&mmulter))
    	print("mmult is ready. Starting...\n");
    else
    {
    	print("mmult is not ready. Exiting...\n\r");
    	exit(-1);
    }
   int a[L],b[L],*c;
	for(i=0;i<L;i++)
	{
		a[i]=1;
		b[i]=2;
	}
    XMmult_Set_a(&mmulter,(u32)a);
    XMmult_Set_b(&mmulter,(u32)b);
    XMmult_Set_group_id_x(&mmulter,0);
    XMmult_Set_group_id_y(&mmulter,0);
    XMmult_Set_group_id_z(&mmulter,0);
    XMmult_Set_global_offset_x(&mmulter,16);
	XMmult_Set_global_offset_x(&mmulter,16);
	XMmult_Set_global_offset_x(&mmulter,0);
	hestonEuro_start(&mmulter);
    XScuTimer_Start(&timer);

    //while(!XMmult_IsDone(&mmulter));
    while(interrupt_get==0);
    print("mmult is finished.\n");
    c=(int*)XMmult_Get_output_r(&mmulter);
    for(i=0;i<L;i++)
    {
    	if(((int)*(c+i))!=32)
    		test++;
    }
    if(test==0)
    	print("test successfully.\n");
    else
    	printf("test failed with %d errors.\n",test);
    xil_printf("Excution time is %d us.\n\n\r",(int)(timePerInterrupt*US*InterruptCounter+0.5));
    cleanup_platform();
    return status;
}
