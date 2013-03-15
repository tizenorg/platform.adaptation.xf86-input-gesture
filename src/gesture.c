/**************************************************************************

xserver-xorg-input-gesture

Copyright (c) 2011 Samsung Electronics Co., Ltd All Rights Reserved

Contact: Sung-Jin Park <sj76.park@samsung.com>
         Sangjin LEE <lsj119@samsung.com>

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sub license, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xorg/inputstr.h>
#include <linux/input.h>
#include <linux/types.h>

#include <xf86_OSproc.h>

#include <unistd.h>

#include <xf86.h>
#include <xf86Xinput.h>
#include <exevents.h>
#include <xorgVersion.h>
#include <xkbsrv.h>

#ifdef HAVE_PROPERTIES
#include <X11/Xatom.h>
#include <xserver-properties.h>
/* 1.6 has properties, but no labels */
#ifdef AXIS_LABEL_PROP
#define HAVE_LABELS
#else
#undef HAVE_LABELS
#endif

#endif

#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <xorg-server.h>
#include <xorgVersion.h>
#include <xf86Module.h>
#include <X11/Xatom.h>
#include "gesture.h"
#include <xorg/mi.h>

//Basic functions
static int GesturePreInit(InputDriverPtr  drv, InputInfoPtr pInfo, int flags);
static void GestureUnInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags);
static pointer GesturePlug(pointer module, pointer options, int *errmaj, int  *errmin);
static void GestureUnplug(pointer p);
static int GestureControl(DeviceIntPtr    device,int what);
static int GestureInit(DeviceIntPtr device);
static void GestureFini(DeviceIntPtr device);
static void GestureReadInput(InputInfoPtr pInfo);

//other initializers
ErrorStatus GestureRegionsInit(void);

//event queue handling functions
ErrorStatus GestureInitEQ(void);
ErrorStatus GestureFiniEQ(void);
ErrorStatus GestureEnqueueEvent(int screen_num, InternalEvent *ev, DeviceIntPtr device);
ErrorStatus GestureEventsFlush(void);
void GestureEventsDrop(void);

//utility functions
ErrorStatus GestureRegionsReinit(void);
void GestureEnable(int enable, Bool prop, DeviceIntPtr dev);
static inline void GestureEnableDisable();
void GestureCbEventsGrabbed(Mask *pGrabMask, GestureGrabEventPtr *pGrabEvent);
void GestureCbEventsSelected(Window win, Mask *pEventMask);
WindowPtr GestureGetEventsWindow(void);
static uint32_t GestureTouchFindDDXIDByTouchID(DeviceIntPtr device, uint32_t touchid);

//Enqueued event handlers and enabler/disabler
static ErrorStatus GestureEnableEventHandler(InputInfoPtr pInfo);
static ErrorStatus GestureDisableEventHandler(void);
static CARD32 GestureTimerHandler(OsTimerPtr timer, CARD32 time, pointer arg);
static CARD32 GestureEventTimerHandler(OsTimerPtr timer, CARD32 time, pointer arg);
void GestureHandleMTSyncEvent(int screen_num, InternalEvent *ev, DeviceIntPtr device);
void GestureHandleTouchBeginEvent(int screen_num, InternalEvent *ev, DeviceIntPtr device);
void GestureHandleTouchUpdateEvent(int screen_num, InternalEvent *ev, DeviceIntPtr device);
void GestureHandleTouchEndEvent(int screen_num, InternalEvent *ev, DeviceIntPtr device);

//Gesture recognizer helper
static Bool PointInBorderSize(WindowPtr pWin, int x, int y);
static WindowPtr GestureWindowOnXY(int x, int y);
Bool GestureHasFingerEventMask(int eventType, int num_finger);
static double get_angle(int x1, int y1, int x2, int y2);

//Gesture recognizer and handlers
void GestureRecognize_GroupPinchRotation(int type, InternalEvent *ev, DeviceIntPtr device, int idx, int timer_expired);
void GestureRecognize_GroupFlick(int type, InternalEvent *ev, DeviceIntPtr device, int idx);
void GestureRecognize_GroupPan(int type, InternalEvent *ev, DeviceIntPtr device, int idx, int timer_expired);
void GestureRecognize_GroupTap(int type, InternalEvent *ev, DeviceIntPtr device, int idx, int timer_expired);
void GestureRecognize_GroupTapNHold(int type, InternalEvent *ev, DeviceIntPtr device, int idx, int timer_expired);
void GestureRecognize_GroupHold(int type, InternalEvent *ev, DeviceIntPtr device, int idx, int timer_expired);
void GestureHandleGesture_Flick(int num_of_fingers, int distance, Time duration, int direction);
void GestureHandleGesture_Tap(int num_finger, int tap_repeat, int cx, int cy);
void GestureHandleGesture_PinchRotation(int num_of_fingers, double zoom, double angle, int distance, int cx, int cy, int kinds);
void GestureHandleGesture_Hold(int num_fingers, int cx, int cy, Time holdtime, int kinds);
void GestureHandleGesture_TapNHold(int num_fingers, int cx, int cy, Time interval, Time holdtime, int kinds);
void GestureHandleGesture_Pan(int num_fingers, short int dx, short int dy, int direction, int distance, Time duration, int kinds);
void GestureRecognize(int type, InternalEvent *ev, DeviceIntPtr device, int idx);
ErrorStatus GestureFlushOrDrop(void);

#ifdef HAVE_PROPERTIES
//function related property handling
static void GestureInitProperty(DeviceIntPtr dev);
static int GestureSetProperty(DeviceIntPtr dev, Atom atom, XIPropertyValuePtr val, BOOL checkonly);
#endif

static Atom prop_gesture_recognizer_onoff = None;

GestureDevicePtr g_pGesture = NULL;
_X_EXPORT InputDriverRec GESTURE = {
    1,
    "gesture",
    NULL,
    GesturePreInit,
    GestureUnInit,
    NULL,
    0
};

static XF86ModuleVersionInfo GestureVersionRec =
{
    "gesture",
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR,
    PACKAGE_VERSION_PATCHLEVEL,
    ABI_CLASS_XINPUT,
    ABI_XINPUT_VERSION,
    MOD_CLASS_XINPUT,
    {0, 0, 0, 0}
};

_X_EXPORT XF86ModuleData gestureModuleData =
{
    &GestureVersionRec,
    &GesturePlug,
    &GestureUnplug
};

static void
printk(const char* fmt, ...)
{
	static FILE* fp = NULL;
	static char init = 0;
	va_list argptr;

	if(!init && !fp)
	{
		fp = fopen("/dev/kmsg", "wt");
		init = 1;
	}

	if(!fp) return;

	va_start(argptr, fmt);
	vfprintf(fp, fmt, argptr);
	fflush(fp);
	va_end(argptr);
}

static Bool
PointInBorderSize(WindowPtr pWin, int x, int y)
{
    BoxRec box;
    if( pixman_region_contains_point (&pWin->borderSize, x, y, &box) )
	return TRUE;

    return FALSE;
}

static WindowPtr
GestureWindowOnXY(int x, int y)
{
    WindowPtr pWin;
    BoxRec box;
    SpritePtr pSprite;
    DeviceIntPtr pDev = g_pGesture->master_pointer;

    pSprite = pDev->spriteInfo->sprite;
    pSprite->spriteTraceGood = 1;	/* root window still there */
    pWin = RootWindow(pDev)->firstChild;

    while (pWin)
    {
	if ((pWin->mapped) &&
	    (x >= pWin->drawable.x - wBorderWidth (pWin)) &&
	    (x < pWin->drawable.x + (int)pWin->drawable.width +
	     wBorderWidth(pWin)) &&
	    (y >= pWin->drawable.y - wBorderWidth (pWin)) &&
	    (y < pWin->drawable.y + (int)pWin->drawable.height +
	     wBorderWidth (pWin))
	    /* When a window is shaped, a further check
	     * is made to see if the point is inside
	     * borderSize
	     */
	    && (!wBoundingShape(pWin) || PointInBorderSize(pWin, x, y))
	    && (!wInputShape(pWin) ||
		RegionContainsPoint(wInputShape(pWin),
				    x - pWin->drawable.x,
				    y - pWin->drawable.y, &box))
#ifdef ROOTLESS
    /* In rootless mode windows may be offscreen, even when
     * they're in X's stack. (E.g. if the native window system
     * implements some form of virtual desktop system).
     */
		&& !pWin->rootlessUnhittable
#endif
	    )
	{
	    if (pSprite->spriteTraceGood >= pSprite->spriteTraceSize)
	    {
		pSprite->spriteTraceSize += 10;
		pSprite->spriteTrace = realloc(pSprite->spriteTrace,
		                    pSprite->spriteTraceSize*sizeof(WindowPtr));
	    }
	    pSprite->spriteTrace[pSprite->spriteTraceGood++] = pWin;
	    pWin = pWin->firstChild;
	}
	else
	    pWin = pWin->nextSib;
    }
    return pSprite->spriteTrace[pSprite->spriteTraceGood-1];
}

Bool
GestureHasFingerEventMask(int eventType, int num_finger)
{
	Bool ret = FALSE;
	Mask eventmask = (1L << eventType);

	if( (g_pGesture->grabMask & eventmask) &&
		(g_pGesture->GrabEvents[eventType].pGestureGrabWinInfo[num_finger].window != None) )
	{
#ifdef __DETAIL_DEBUG__
		ErrorF("[X11][GestureHasFingerEventMask] TRUE !! Has grabMask\n");
#endif//__DETAIL_DEBUG__
		return TRUE;
	}

	if( g_pGesture->eventMask & eventmask )
	{
#ifdef __DETAIL_DEBUG__
		ErrorF("[X11][GestureHasFingerEventMask] TRUE !! Has eventMask\n");
#endif//__DETAIL_DEBUG__
		return TRUE;
	}

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureHasFingerEventMask] FALSE !! eventType=%d, num_finger=%d\n", eventType, num_finger);
#endif//__DETAIL_DEBUG__

	return ret;
}

static double
get_angle(int x1, int y1, int x2, int y2)
{
   double a, xx, yy;
   xx = fabs(x2 - x1);
   yy = fabs(y2 - y1);

   if (((int) xx) && ((int) yy))
     {
        a = atan(yy / xx);
        if (x1 < x2)
          {
             if (y1 < y2)
               {
                  return (RAD_360DEG - a);
               }
             else
               {
                  return (a);
               }
          }
        else
          {
             if (y1 < y2)
               {
                  return (RAD_180DEG + a);
               }
             else
               {
                  return (RAD_180DEG - a);
               }
          }
     }

   if (((int) xx))
     {  /* Horizontal line */
        if (x2 < x1)
          {
             return (RAD_180DEG);
          }
        else
          {
             return (0.0);
          }
     }

   /* Vertical line */
   if (y2 < y1)
     {
        return (RAD_90DEG);
     }
   else
     {
        return (RAD_270DEG);
     }
}

void
GestureHandleGesture_Flick(int num_of_fingers, int distance, Time duration, int direction)
{
	Window target_win;
	WindowPtr target_pWin;
	xGestureNotifyFlickEvent fev;

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureHandleGesture_Flick] num_fingers=%d, distance=%d, duration=%d, direction=%d\n",
		num_of_fingers, distance, duration, direction);
#endif//__DETAIL_DEBUG__

	g_pGesture->recognized_gesture |= FlickFilterMask;
	memset(&fev, 0, sizeof(xGestureNotifyFlickEvent));
	fev.type = GestureNotifyFlick;
	fev.kind = GestureDone;
	fev.num_finger = num_of_fingers;
	fev.distance = distance;
	fev.duration = duration;
	fev.direction = direction;

	target_win = g_pGesture->GrabEvents[GestureNotifyFlick].pGestureGrabWinInfo[num_of_fingers].window;
	target_pWin = g_pGesture->GrabEvents[GestureNotifyFlick].pGestureGrabWinInfo[num_of_fingers].pWin;

	if( g_pGesture->grabMask && (target_win != None) )
	{
		fev.window = target_win;
	}
	else
	{
		fev.window = g_pGesture->gestureWin;
	}

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureHandleGesture_Flick] fev.window=0x%x, g_pGesture->grabMask=0x%x\n", fev.window, g_pGesture->grabMask);
#endif//__DETAIL_DEBUG__

	GestureSendEvent(target_pWin, GestureNotifyFlick, GestureFlickMask, (xGestureCommonEvent *)&fev);
}

void
GestureHandleGesture_Tap(int num_finger, int tap_repeat, int cx, int cy)
{
	Window target_win;
	WindowPtr target_pWin;
	xGestureNotifyTapEvent tev;

	//skip non-tap events and single finger tap
	if( !tap_repeat || num_finger <= 1 )
		return;

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureHandleGesture_Tap] num_finger=%d, tap_repeat=%d, cx=%d, cy=%d\n",
		num_finger, tap_repeat, cx, cy);
#endif//__DETAIL_DEBUG__

	g_pGesture->recognized_gesture |= TapFilterMask;
	memset(&tev, 0, sizeof(xGestureNotifyTapEvent));
	tev.type = GestureNotifyTap;
	tev.kind = GestureDone;
	tev.num_finger = num_finger;
	tev.tap_repeat = tap_repeat;
	tev.interval = 0;
	tev.cx = cx;
	tev.cy = cy;

	target_win = g_pGesture->GrabEvents[GestureNotifyTap].pGestureGrabWinInfo[num_finger].window;
	target_pWin = g_pGesture->GrabEvents[GestureNotifyTap].pGestureGrabWinInfo[num_finger].pWin;

	if( g_pGesture->grabMask && (target_win != None) )
	{
		tev.window = target_win;
	}
	else
	{
		tev.window = g_pGesture->gestureWin;
	}

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureHandleGesture_Tap] tev.window=0x%x, g_pGesture->grabMask=0x%x\n", tev.window, g_pGesture->grabMask);
#endif//__DETAIL_DEBUG__

	GestureSendEvent(target_pWin, GestureNotifyTap, GestureTapMask, (xGestureCommonEvent *)&tev);
}

void GestureHandleGesture_PinchRotation(int num_of_fingers, double zoom, double angle, int distance, int cx, int cy, int kinds)
{
	Window target_win;
	WindowPtr target_pWin;
	xGestureNotifyPinchRotationEvent prev;

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureHandleGesture_PinchRotation] num_fingers=%d, zoom=%.2f, angle=%.2f(deg=%.2f), distance=%d, cx=%d, cy=%d\n",
				num_of_fingers, zoom, angle, rad2degree(angle), distance, cx, cy);
#endif//__DETAIL_DEBUG__

	g_pGesture->recognized_gesture |= PinchRotationFilterMask;
	memset(&prev, 0, sizeof(xGestureNotifyPinchRotationEvent));
	prev.type = GestureNotifyPinchRotation;
	prev.kind = kinds;
	prev.num_finger = num_of_fingers;
	prev.zoom = XDoubleToFixed(zoom);
	prev.angle = XDoubleToFixed(angle);
	prev.distance = distance;
	prev.cx = cx;
	prev.cy = cy;

	target_win = g_pGesture->GrabEvents[GestureNotifyPinchRotation].pGestureGrabWinInfo[num_of_fingers].window;
	target_pWin = g_pGesture->GrabEvents[GestureNotifyPinchRotation].pGestureGrabWinInfo[num_of_fingers].pWin;

	if( g_pGesture->grabMask && (target_win != None) )
	{
		prev.window = target_win;
	}
	else
	{
		prev.window = g_pGesture->gestureWin;
	}

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureHandleGesture_PinchRotation] prev.window=0x%x, g_pGesture->grabMask=0x%x\n", (unsigned int)prev.window, (unsigned int)g_pGesture->grabMask);
#endif//__DETAIL_DEBUG__

	GestureSendEvent(target_pWin, GestureNotifyPinchRotation, GesturePinchRotationMask, (xGestureCommonEvent *)&prev);
}

void GestureHandleGesture_Hold(int num_fingers, int cx, int cy, Time holdtime, int kinds)
{
	Window target_win;
	WindowPtr target_pWin;
	xGestureNotifyHoldEvent hev;

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureHandleGesture_Hold] num_fingers=%d, cx=%d, cy=%d, holdtime=%d, kinds=%d\n",
				num_fingers, cx, cy, holdtime, kinds);
#endif//__DETAIL_DEBUG__

	g_pGesture->recognized_gesture |= HoldFilterMask;
	memset(&hev, 0, sizeof(xGestureNotifyHoldEvent));
	hev.type = GestureNotifyHold;
	hev.kind = kinds;
	hev.num_finger = num_fingers;
	hev.holdtime = holdtime;
	hev.cx = cx;
	hev.cy = cy;

	target_win = g_pGesture->GrabEvents[GestureNotifyHold].pGestureGrabWinInfo[num_fingers].window;
	target_pWin = g_pGesture->GrabEvents[GestureNotifyHold].pGestureGrabWinInfo[num_fingers].pWin;

	if( g_pGesture->grabMask && (target_win != None) )
	{
		hev.window = target_win;
	}
	else
	{
		hev.window = g_pGesture->gestureWin;
	}

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureHandleGesture_Hold] hev.window=0x%x, g_pGesture->grabMask=0x%x\n", hev.window, g_pGesture->grabMask);
#endif//__DETAIL_DEBUG__

	GestureSendEvent(target_pWin, GestureNotifyHold, GestureHoldMask, (xGestureCommonEvent *)&hev);
}

void GestureHandleGesture_TapNHold(int num_fingers, int cx, int cy, Time interval, Time holdtime, int kinds)
{
	Window target_win;
	WindowPtr target_pWin;
	xGestureNotifyTapNHoldEvent thev;

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureHandleGesture_TapNHold] num_fingers=%d, cx=%d, cy=%d, interval=%d, holdtime=%d, kinds=%d\n",
				num_fingers, cx, cy, interval, holdtime, kinds);
#endif//__DETAIL_DEBUG__

	g_pGesture->recognized_gesture |= TapNHoldFilterMask;
	memset(&thev, 0, sizeof(xGestureNotifyTapNHoldEvent));
	thev.type = GestureNotifyTapNHold;
	thev.kind = kinds;
	thev.num_finger = num_fingers;
	thev.holdtime = holdtime;
	thev.cx = cx;
	thev.cy = cy;
	thev.interval = interval;

	target_win = g_pGesture->GrabEvents[GestureNotifyTapNHold].pGestureGrabWinInfo[num_fingers].window;
	target_pWin = g_pGesture->GrabEvents[GestureNotifyTapNHold].pGestureGrabWinInfo[num_fingers].pWin;

	if( g_pGesture->grabMask && (target_win != None) )
	{
		thev.window = target_win;
	}
	else
	{
		thev.window = g_pGesture->gestureWin;
	}

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureHandleGesture_TapNHold] thev.window=0x%x, g_pGesture->grabMask=0x%x\n", thev.window, g_pGesture->grabMask);
#endif//__DETAIL_DEBUG__

	GestureSendEvent(target_pWin, GestureNotifyTapNHold, GestureTapNHoldMask, (xGestureCommonEvent *)&thev);
}

void GestureHandleGesture_Pan(int num_fingers, short int dx, short int dy, int direction, int distance, Time duration, int kinds)
{
	Window target_win;
	WindowPtr target_pWin;
	xGestureNotifyPanEvent pev;

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureHandleGesture_Pan] num_fingers=%d, dx=%d, dy=%d, direction=%d, distance=%d, duration=%d, kinds=%d\n",
				num_fingers, dx, dy, direction, distance, duration, kinds);
#endif//__DETAIL_DEBUG__

	g_pGesture->recognized_gesture |= PanFilterMask;
	memset(&pev, 0, sizeof(xGestureNotifyPanEvent));
	pev.type = GestureNotifyPan;
	pev.kind = kinds;
	pev.num_finger = num_fingers;
	pev.direction = direction;
	pev.distance = distance;
	pev.duration = duration;
	pev.dx = dx;
	pev.dy = dy;

	target_win = g_pGesture->GrabEvents[GestureNotifyPan].pGestureGrabWinInfo[num_fingers].window;
	target_pWin = g_pGesture->GrabEvents[GestureNotifyPan].pGestureGrabWinInfo[num_fingers].pWin;

	if( g_pGesture->grabMask && (target_win != None) )
	{
		pev.window = target_win;
	}
	else
	{
		pev.window = g_pGesture->gestureWin;
	}

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureHandleGesture_Pan] pev.window=0x%x, g_pGesture->grabMask=0x%x\n", pev.window, g_pGesture->grabMask);
#endif//__DETAIL_DEBUG__

	GestureSendEvent(target_pWin, GestureNotifyPan, GesturePanMask, (xGestureCommonEvent *)&pev);
}

void
GestureRecognize_GroupPinchRotation(int type, InternalEvent *ev, DeviceIntPtr device, int idx, int timer_expired)
{
	static int cx, cy;

	static int num_pressed = 0;
	static int state = GestureEnd;
	static int event_type = GestureNotifyPinchRotation;
	static OsTimerPtr pinchrotation_event_timer = NULL;

	static pixman_region16_t base_area;
	static pixman_region16_t cur_area;

	static double base_distance = 0.0f;
	static double base_angle = 0.0f;

	static double prev_distance = 0.0f;
	static double prev_angle = 0.0f;

	static double cur_distance = 0.0f;
	static double cur_angle = 0.0f;

	double diff_distance = 0.0f;
	double diff_angle = 0.0f;

	static int has_event_mask = 0;

	static Time base_time = 0;
	Time current_time;

	if( timer_expired )
	{
		if( state == GestureEnd )
		{
			current_time = GetTimeInMillis();
			if( (current_time - base_time) >= PINCHROTATION_TIME_THRESHOLD )
			{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupPinchRotation][Timer] You must move farther than dist threshold(=%.2f) or angle threshold(=%2f) within time threshold(=%d) !\n", PINCHROTATION_DIST_THRESHOLD, PINCHROTATION_ANGLE_THRESHOLD, PINCHROTATION_TIME_THRESHOLD);
#endif//__DETAIL_DEBUG__
				goto cleanup_pinchrotation;
			}
		}

		return;
	}

	switch( type )
	{
		case ET_TouchBegin:
			g_pGesture->fingers[idx].flags |= PressFlagPinchRotation;

			if( g_pGesture->num_pressed < 2 )
				return;

			if( g_pGesture->num_pressed < num_pressed && state != GestureEnd )
			{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupPinchRotation][P][cleanup] num_finger changed ! num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
				goto cleanup_pinchrotation;
			}

			if( base_distance == 0.0f && g_pGesture->num_pressed == 2 )
			{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupPinchRotation][First Time !!!] num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__

				base_time = GetTimeInMillis();
				pixman_region_init(&base_area);
				pixman_region_union(&base_area, &g_pGesture->finger_rects[0], &g_pGesture->finger_rects[1]);

				prev_distance = base_distance = AREA_DIAG_LEN(&base_area.extents);

#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupPinchRotation][P] x1=%d, x2=%d, y1=%d, y2=%d\n", g_pGesture->fingers[0].px, g_pGesture->fingers[1].px,
				g_pGesture->fingers[0].py, g_pGesture->fingers[1].py);
#endif//__DETAIL_DEBUG__

				prev_angle = base_angle = get_angle(g_pGesture->fingers[0].px, g_pGesture->fingers[0].py, g_pGesture->fingers[1].px, g_pGesture->fingers[1].py);
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupPinchRotation][P] base_angle=%.2f(deg=%.2f)\n", base_angle, rad2degree(base_angle));
#endif//__DETAIL_DEBUG__
				event_type = GestureNotifyPinchRotation;
				pinchrotation_event_timer = TimerSet(pinchrotation_event_timer, 0, PINCHROTATION_TIME_THRESHOLD, GestureEventTimerHandler, (int *)&event_type);
			}
			num_pressed = g_pGesture->num_pressed;

#ifdef __DETAIL_DEBUG__
			ErrorF("[GroupPinchRotation][P][num_pressed=%d] AREA_SIZE(base_area.extents)=%d\n", num_pressed, AREA_SIZE(&base_area.extents));
			ErrorF("[GroupPinchRotation][P][num_pressed=%d] base_distance=%.2f, base_angle=%.2f(deg=%.2f)\n", num_pressed, base_distance, base_angle, rad2degree(base_angle));
#endif//__DETAIL_DEBUG__
			break;

		case ET_TouchUpdate:
			if( !(g_pGesture->fingers[idx].flags & PressFlagPinchRotation) )
				break;

			if( (num_pressed != g_pGesture->num_pressed) && (state != GestureEnd) )
			{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupPinchRotation][M][cleanup] num_finger changed ! num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
				goto cleanup_pinchrotation;
			}

			if( num_pressed < 2 )
				return;

			if( g_pGesture->fingers[0].mx && g_pGesture->fingers[0].my && g_pGesture->fingers[1].mx && g_pGesture->fingers[1].my )
			{
				pixman_region_init(&cur_area);
				pixman_region_union(&cur_area, &g_pGesture->finger_rects[0], &g_pGesture->finger_rects[1]);

				cur_distance = AREA_DIAG_LEN(&cur_area.extents);

#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupPinchRotation][M] x1=%d, x2=%d, y1=%d, y2=%d\n", g_pGesture->fingers[0].mx, g_pGesture->fingers[1].mx,
				g_pGesture->fingers[0].my, g_pGesture->fingers[1].my);
#endif//__DETAIL_DEBUG__

				cur_angle = get_angle(g_pGesture->fingers[0].mx, g_pGesture->fingers[0].my, g_pGesture->fingers[1].mx, g_pGesture->fingers[1].my);
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupPinchRotation][M] cur_angle=%.2f(deg=%.2f)\n", cur_angle, rad2degree(cur_angle));
#endif//__DETAIL_DEBUG__

				diff_distance = prev_distance - cur_distance;
				diff_angle = prev_angle - cur_angle;

				cx = AREA_CENTER_X(&cur_area.extents);
				cy = AREA_CENTER_Y(&cur_area.extents);

#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupPinchRotation][M][state=%d] cx=%d, cy=%d\n", state, cx, cy);
#endif//__DETAIL_DEBUG__

#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupPinchRotation][M][num_pressed=%d] prev_distance=%.2f, cur_distance=%.2f, diff=%.2f\n", num_pressed, prev_distance, cur_distance, diff_distance);
				ErrorF("[GroupPinchRotation][M][num_pressed=%d] prev_angle=%.2f(deg=%.2f), cur_angle=%.2f(deg=%.2f), diff=%.2f(deg=%.2f)\n", num_pressed, prev_angle, rad2degree(prev_angle), cur_angle, rad2degree(cur_angle), diff_angle, rad2degree(diff_angle));
#endif//__DETAIL_DEBUG__

				switch( state )
				{
					case GestureEnd:
						if( (ABS(diff_distance) >= PINCHROTATION_DIST_THRESHOLD) || (ABS(diff_angle) >= PINCHROTATION_ANGLE_THRESHOLD) )
						{
#ifdef __DETAIL_DEBUG__
							if( ABS(diff_distance) >= PINCHROTATION_DIST_THRESHOLD )
								ErrorF("[GroupPinchRotation][M] zoom changed !\n");

							if( ABS(diff_angle) >= PINCHROTATION_ANGLE_THRESHOLD )
								ErrorF("[GroupPinchRotation][M] angle changed !\n");
#endif//__DETAIL_DEBUG__

							TimerCancel(pinchrotation_event_timer);
							state = GestureBegin;
							goto gesture_begin_handle;
						}
						break;

					case GestureBegin:
gesture_begin_handle:
#ifdef __DETAIL_DEBUG__
						ErrorF("[GroupPinchRotation] PINCHROTATION Begin !cx=%d, cy=%d, state=%d\n", cx, cy, state);
#endif//__DETAIL_DEBUG__
						if( GestureHasFingerEventMask(GestureNotifyPinchRotation, num_pressed) )
						{
							GestureHandleGesture_PinchRotation(num_pressed, cur_distance / base_distance, (cur_angle > base_angle) ? (cur_angle-base_angle) : (RAD_360DEG + cur_angle - base_angle), cur_distance, cx, cy, GestureBegin);
							prev_distance = cur_distance;
							prev_angle = cur_angle;
							state = GestureUpdate;
							has_event_mask = 1;
						}
						else
						{
							has_event_mask = 0;
							goto cleanup_pinchrotation;
						}
						break;

					case GestureUpdate:
						//if( ABS(diff_distance) < PINCHROTATION_DIST_THRESHOLD && ABS(diff_angle) < PINCHROTATION_ANGLE_THRESHOLD )
						//	break;

#ifdef __DETAIL_DEBUG__
						if( ABS(diff_distance) >= PINCHROTATION_DIST_THRESHOLD )
							ErrorF("[GroupPinchRotation][M] zoom changed !\n");

						if( ABS(diff_angle) >= PINCHROTATION_ANGLE_THRESHOLD )
							ErrorF("[GroupPinchRotation][M] angle changed !\n");
#endif//__DETAIL_DEBUG__

#ifdef __DETAIL_DEBUG__
						ErrorF("[GroupPinchRotation] PINCHROTATION Update ! cx=%d, cy=%d, state=%d\n", cx, cy, state);
#endif//__DETAIL_DEBUG__
						GestureHandleGesture_PinchRotation(num_pressed, cur_distance / base_distance, (cur_angle > base_angle) ? (cur_angle-base_angle) : (RAD_360DEG + cur_angle - base_angle), cur_distance, cx, cy, GestureUpdate);
						prev_distance = cur_distance;
						prev_angle = cur_angle;
						break;

					case GestureDone:
					default:
						break;
				}
			}
			break;

		case ET_TouchEnd:
			if( state != GestureEnd && num_pressed >= 2)
			{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupPinchRotation][R][cleanup] num_finger changed ! num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
				goto cleanup_pinchrotation;
			}

			if( g_pGesture->num_pressed )
				break;

			goto cleanup_pinchrotation;
			break;
	}

	return;

cleanup_pinchrotation:

	if(  has_event_mask  && (state == GestureBegin || state == GestureUpdate) )
	{
		state = GestureEnd;
#ifdef __DETAIL_DEBUG__
		ErrorF("[GroupPinchRotation] PINCHROTATION End ! cx=%d, cy=%d, state=%d\n", cx, cy, state);
#endif//__DETAIL_DEBUG__
		GestureHandleGesture_PinchRotation(num_pressed, cur_distance / base_distance, (cur_angle > base_angle) ? (cur_angle-base_angle) : (RAD_360DEG + cur_angle - base_angle), cur_distance, cx, cy, GestureEnd);
	}
	else
	{
		g_pGesture->recognized_gesture &= ~PinchRotationFilterMask;
	}

	g_pGesture->filter_mask |= PinchRotationFilterMask;

	if( g_pGesture->filter_mask == GESTURE_FILTER_MASK_ALL )
	{
#ifdef __DETAIL_DEBUG__
		ErrorF("[GroupPinchRotation][cleanup] GestureFlushOrDrop() !\n");
#endif//__DETAIL_DEBUG__

		if( ERROR_INVALPTR == GestureFlushOrDrop() )
		{
			GestureControl(g_pGesture->this_device, DEVICE_OFF);
		}
	}

	prev_distance = base_distance = 0.0f;
	prev_angle = base_angle = 0.0f;
	has_event_mask = num_pressed = 0;
	state = GestureEnd;
	cx = cy = 0;
	TimerCancel(pinchrotation_event_timer);
	return;
}

void
GestureRecognize_GroupFlick(int type, InternalEvent *ev, DeviceIntPtr device, int idx)
{
	static int num_pressed = 0;
	static int mbits = 0;
	static int base_area_size = 0;
	static Time base_time = 0;
	static int base_x, base_y;
	Time current_time;
	Time duration;
	int distx, disty;
	int distance, direction;
	int area_size;
	int flicked = 0;

	switch( type )
	{
		case ET_TouchBegin:
			g_pGesture->fingers[idx].flags |= PressFlagFlick;

			if( g_pGesture->num_pressed < 2 )
				return;

			if( !base_area_size || g_pGesture->num_pressed > num_pressed )
			{
				base_area_size = AREA_SIZE(&g_pGesture->area.extents);
				base_x = g_pGesture->area.extents.x1;
				base_y = g_pGesture->area.extents.y1;
				base_time = GetTimeInMillis();
			}
			num_pressed = g_pGesture->num_pressed;

#ifdef __DETAIL_DEBUG__
			ErrorF("[GroupFlick][P]][num_pressed=%d] AREA_SIZE(area.extents)=%d\n", num_pressed, base_area_size);
#endif//__DETAIL_DEBUG__
			break;

		case ET_TouchUpdate:
			if( !(g_pGesture->fingers[idx].flags & PressFlagFlick ) )
				break;

#ifdef __DETAIL_DEBUG__
			if( num_pressed > g_pGesture->num_pressed )
			{
				ErrorF("[GroupFlick][M][cleanup] num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
				//goto cleanup_flick;
			}
#endif//__DETAIL_DEBUG__

			if( num_pressed < 2 )
				return;

			mbits |= (1 << idx);
			if( mbits == (pow(2, num_pressed)-1) )
			{
				area_size = AREA_SIZE(&g_pGesture->area.extents);
#ifdef __DETAIL_DEBUG__
				ErrorF("[M][num_pressed=%d] AREA_SIZE(area.extents)=%d\n", num_pressed, area_size);
#endif//__DETAIL_DEBUG__
				if( ABS(base_area_size - area_size) >= FLICK_AREA_THRESHOLD )
				{
#ifdef __DETAIL_DEBUG__
					ErrorF("[GroupFlick][M] diff between Area size(=%d) and base area size(=%d) is bigger than threshold(=%d)!\n", area_size, base_area_size, FLICK_AREA_THRESHOLD);
#endif//__DETAIL_DEBUG__
					goto cleanup_flick;
				}

				current_time = GetTimeInMillis();
				if( (current_time - base_time) >= FLICK_AREA_TIMEOUT )
				{
#ifdef __DETAIL_DEBUG__
					ErrorF("[GroupFlick][M] diff between current time(=%d) and base time(=%d) is bigger than threashold(=%d) !\n", current_time, base_time, FLICK_AREA_TIMEOUT);
#endif//__DETAIL_DEBUG__
					goto cleanup_flick;
				}
				mbits = 0;
			}
			break;

		case ET_TouchEnd:
			if( g_pGesture->num_pressed )
				break;

			duration = GetTimeInMillis() - base_time;
			distx = g_pGesture->area.extents.x1 - base_x;
			disty = g_pGesture->area.extents.y1 - base_y;

#ifdef __DETAIL_DEBUG__
			ErrorF("[GroupFlick] duration=%d, distx=%d, disty=%d\n", duration, distx, disty);
#endif//__DETAIL_DEBUG__

			if( duration <= 0 || duration >= FLICK_AREA_TIMEOUT )
				goto cleanup_flick;

			if( ABS(distx) >= FLICK_MOVE_THRESHOLD )
			{
				direction = (distx > 0) ? FLICK_EASTWARD : FLICK_WESTWARD;
				distance = ABS(distx);
				flicked++;
			}
			else if( ABS(disty) >= FLICK_MOVE_THRESHOLD )
			{
				direction = (disty > 0) ? FLICK_SOUTHWARD : FLICK_NORTHWARD;
				distance = ABS(disty);
				flicked++;
			}

			if( !flicked )
				goto cleanup_flick;

			if( GestureHasFingerEventMask(GestureNotifyFlick, num_pressed) )
				GestureHandleGesture_Flick(num_pressed, distance, duration, direction);
			goto cleanup_flick_recognized;
			break;
	}

	return;

cleanup_flick:

	g_pGesture->recognized_gesture &= ~FlickFilterMask;

cleanup_flick_recognized:

	g_pGesture->filter_mask |= FlickFilterMask;
	num_pressed = 0;
	base_area_size = 0;
	base_time = 0;
	mbits = 0;
	return;
}

void
GestureRecognize_GroupPan(int type, InternalEvent *ev, DeviceIntPtr device, int idx, int timer_expired)
{
	static int num_pressed = 0;
	static int mbits = 0;
	static int base_area_size = 0;
	static Time base_time = 0;
	static pixman_box16_t base_box_ext;
	static int base_cx;
	static int base_cy;
	static int prev_cx;
	static int prev_cy;
	static int cx = 0;
	static int cy = 0;
	int dx, dy;
	static Time prev_time = 0;
	Time current_time = 0;
	int distance = 0;
	int direction = 0;
	int area_size;
	static int time_checked = 0;
	static int state = GestureEnd;

	static OsTimerPtr pan_event_timer = NULL;
	static int event_type = GestureNotifyPan;

	if( timer_expired )
	{
		if( !time_checked )
		{
			current_time = GetTimeInMillis();
			if( (current_time - base_time) >= PAN_TIME_THRESHOLD )
			{
				if( (!cx && !cy) || INBOX(&base_box_ext, cx, cy) )
				{
#ifdef __DETAIL_DEBUG__
					ErrorF("[GroupPan][Timer] You must move farther than move threshold(=%d) within time threshold(=%d) !\n", PAN_MOVE_THRESHOLD*2, PAN_TIME_THRESHOLD);
#endif//__DETAIL_DEBUG__
					goto cleanup_pan;
				}
				time_checked = 1;
			}
		}
		return;
	}

	switch( type )
	{
		case ET_TouchBegin:
			g_pGesture->fingers[idx].flags |= PressFlagPan;

			if( g_pGesture->num_pressed < 2 )
				return;

			if( !base_area_size || g_pGesture->num_pressed > num_pressed )
			{
				if( state != GestureEnd )
				{
#ifdef __DETAIL_DEBUG__
					ErrorF("[GroupPan][P][cleanup] num_finger changed ! num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
					goto cleanup_pan;
				}
				base_area_size = AREA_SIZE(&g_pGesture->area.extents);
				prev_cx = base_cx = AREA_CENTER_X(&g_pGesture->area.extents);
				prev_cy = base_cy = AREA_CENTER_Y(&g_pGesture->area.extents);
				prev_time = base_time = GetTimeInMillis();
				base_box_ext.x1 = base_cx-PAN_MOVE_THRESHOLD;
				base_box_ext.y1 = base_cy-PAN_MOVE_THRESHOLD;
				base_box_ext.x2 = base_cx+PAN_MOVE_THRESHOLD;
				base_box_ext.y2 = base_cy+PAN_MOVE_THRESHOLD;
				event_type = GestureNotifyPan;
				pan_event_timer = TimerSet(pan_event_timer, 0, PAN_TIME_THRESHOLD, GestureEventTimerHandler, (int *)&event_type);
			}
			num_pressed = g_pGesture->num_pressed;

#ifdef __DETAIL_DEBUG__
			ErrorF("[GroupPan][P][num_pressed=%d] AREA_SIZE(area.extents)=%d, base_cx=%d, base_cy=%d\n", num_pressed, base_area_size, base_cx, base_cy);
#endif//__DETAIL_DEBUG__
			break;

		case ET_TouchUpdate:
			if( !(g_pGesture->fingers[idx].flags & PressFlagPan ) )
				break;

			if( num_pressed != g_pGesture->num_pressed )
			{
				if( state != GestureEnd )
				{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupPan][M][cleanup] num_finger changed ! num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
					goto cleanup_pan;
				}
			}

			if( num_pressed < 2 )
				return;

			mbits |= (1 << idx);
			if( mbits == (pow(2, num_pressed)-1) )
			{
				area_size = AREA_SIZE(&g_pGesture->area.extents);
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupPan][M][num_pressed=%d] area_size=%d, base_area_size=%d, diff=%d\n", num_pressed, area_size, base_area_size, ABS(base_area_size - area_size));
#endif//__DETAIL_DEBUG__

				if( (state != GestureUpdate) && (ABS(base_area_size - area_size) >= PAN_AREA_THRESHOLD) )
				{
#ifdef __DETAIL_DEBUG__
					ErrorF("[GroupPan][M] diff between area size(=%d) and base area size(=%d) is bigger than threshold(=%d)!\n", area_size, base_area_size, PAN_AREA_THRESHOLD);
#endif//__DETAIL_DEBUG__
					goto cleanup_pan;
				}

				cx = AREA_CENTER_X(&g_pGesture->area.extents);
				cy = AREA_CENTER_Y(&g_pGesture->area.extents);

#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupPan][M] cx=%d, prev_cx=%d, diff=%d\n", cx, prev_cx, ABS(cx-prev_cx));
				ErrorF("[GroupPan][M] cy=%d, prev_cy=%d, diff=%d\n", cy, prev_cy, ABS(cy-prev_cy));
#endif//__DETAIL_DEBUG__

				if( state <= GestureBegin )
				{
					if( !INBOX(&base_box_ext, cx, cy) )
					{
						TimerCancel(pan_event_timer);
						pan_event_timer = NULL;
						
						if( GestureHasFingerEventMask(GestureNotifyPan, num_pressed) )
						{
							GestureHandleGesture_Pan(num_pressed, prev_cx, prev_cy, direction, distance, current_time-prev_time, GestureBegin);
							state = GestureUpdate;
						}
						else
							goto cleanup_pan;
					}
				}
				else
				{
					dx = cx-prev_cx;
					dy = cy-prev_cy;

					//if( ABS(dx) >= PAN_UPDATE_MOVE_THRESHOLD || ABS(dy) >= PAN_UPDATE_MOVE_THRESHOLD )
					{
#ifdef __DETAIL_DEBUG__
						ErrorF("[GroupPan] PAN Update !dx=%d, dy=%d, state=%d\n", dx, dy, state);
#endif//__DETAIL_DEBUG__

						GestureHandleGesture_Pan(num_pressed, dx, dy, direction, distance, current_time-prev_time, GestureUpdate);
					}
				}

				prev_cx = cx;
				prev_cy = cy;
				mbits = 0;
			}
			break;

		case ET_TouchEnd:
			if( state != GestureEnd && num_pressed >= 2)
			{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupPan][R][cleanup] num_finger changed ! num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
				goto cleanup_pan;
			}

			if( g_pGesture->num_pressed )
				break;

			goto cleanup_pan;
			break;
	}

	return;

cleanup_pan:

	if( state == GestureBegin || state == GestureUpdate )
	{
		state = GestureEnd;
		if( GestureHasFingerEventMask(GestureNotifyPan, num_pressed) )
		{
			GestureHandleGesture_Pan(num_pressed, (short int)(cx-prev_cx), (short int)(cy-prev_cy), direction, distance, GetTimeInMillis()-prev_time, GestureEnd);
		}
	}
	else
	{
		g_pGesture->recognized_gesture &= ~PanFilterMask;
	}

	g_pGesture->filter_mask |= PanFilterMask;
	num_pressed = 0;
	base_area_size = 0;
	base_time = 0;
	mbits = 0;
	time_checked = 0;
	state = GestureEnd;
	cx = cy = 0;
	prev_time = 0;
	base_box_ext.x1 = base_box_ext.x2 = base_box_ext.y1 = base_box_ext.y2 = 0;
	if( pan_event_timer )
	{
		TimerCancel(pan_event_timer);
		pan_event_timer = NULL;
	}
	return;
}

void
GestureRecognize_GroupTap(int type, InternalEvent *ev, DeviceIntPtr device, int idx, int timer_expired)
{
	static int num_pressed = 0;
	static int base_area_size = 0;

	static Time base_time = 0;
	Time current_time;

	int cx, cy;
	int area_size;

	static int state = 0;
	static int mbits = 0;
	static int base_cx;
	static int base_cy;
	static pixman_box16_t base_box_ext;

	static int tap_repeat = 0;
	static int prev_tap_repeat = 0;
	static int prev_num_pressed = 0;

	static OsTimerPtr tap_event_timer = NULL;
	static int event_type = GestureNotifyTap;

	if( timer_expired )
	{
#ifdef __DETAIL_DEBUG__
		ErrorF("[GroupTap][Timer] state=%d\n", state);
#endif//__DETAIL_DEBUG__

		switch( state )
		{
			case 1://first tap initiation check
				if( num_pressed )
				{
#ifdef __DETAIL_DEBUG__
					ErrorF("[GroupTap][Timer][state=1] Tap time expired !(num_pressed=%d, tap_repeat=%d)\n", num_pressed, tap_repeat);
#endif//__DETAIL_DEBUG__
					state = 0;
					goto cleanup_tap;
				}
				break;

			case 2:
				if( tap_repeat <= 1 )
				{
#ifdef __DETAIL_DEBUG__
					ErrorF("[GroupTap][Timer][state=2] %d finger SINGLE TAP !(ignored)\n", prev_num_pressed);
#endif//__DETAIL_DEBUG__
					state = 0;
					goto cleanup_tap;
				}

#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupTap][Timer][state=2]  tap_repeat=%d, prev_tap_repeat=%d, num_pressed=%d\n", tap_repeat, prev_tap_repeat, num_pressed);
#endif//__DETAIL_DEBUG__
				if( GestureHasFingerEventMask(GestureNotifyTap, prev_num_pressed) )
					GestureHandleGesture_Tap(prev_num_pressed, tap_repeat, base_cx, base_cy);
				goto cleanup_tap;
				break;
		}

		return;
	}

	switch( type )
	{
		case ET_TouchBegin:
			g_pGesture->fingers[idx].flags |= PressFlagTap;

			if( g_pGesture->num_pressed < 2 )
				return;

			if( !prev_num_pressed && (!base_area_size || g_pGesture->num_pressed > num_pressed) )
			{
				base_area_size = AREA_SIZE(&g_pGesture->area.extents);
				base_cx = AREA_CENTER_X(&g_pGesture->area.extents);
				base_cy = AREA_CENTER_Y(&g_pGesture->area.extents);
				base_time = GetTimeInMillis();
				base_box_ext.x1 = base_cx-TAP_MOVE_THRESHOLD;
				base_box_ext.y1 = base_cy-TAP_MOVE_THRESHOLD;
				base_box_ext.x2 = base_cx+TAP_MOVE_THRESHOLD;
				base_box_ext.y2 = base_cy+TAP_MOVE_THRESHOLD;
				state = 1;
				TimerCancel(tap_event_timer);
				tap_event_timer = TimerSet(tap_event_timer, 0, SGL_TAP_TIME_THRESHOLD, GestureEventTimerHandler, (int *)&event_type);
			}

			num_pressed = g_pGesture->num_pressed;

			current_time = GetTimeInMillis();

#ifdef __DETAIL_DEBUG__
			ErrorF("[GroupTap][P][num_pressed=%d] AREA_SIZE(area.extents)=%d, base_cx=%d, base_cy=%d, base_time=%d, current_time=%d\n", num_pressed, base_area_size, base_cx, base_cy, base_time, current_time);
#endif//__DETAIL_DEBUG__
			break;

		case ET_TouchUpdate:
			if( !(g_pGesture->fingers[idx].flags & PressFlagTap ) )
				break;

			if( num_pressed < 2 )
				return;

			if( num_pressed != g_pGesture->num_pressed )
			{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupTap][M][cleanup] num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
				//goto cleanup_tap;
			}

			mbits |= (1 << idx);
			if( mbits == (pow(2, num_pressed)-1) )
			{
				area_size = AREA_SIZE(&g_pGesture->area.extents);
				cx = AREA_CENTER_X(&g_pGesture->area.extents);
				cy = AREA_CENTER_Y(&g_pGesture->area.extents);
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupTap][M][num_pressed=%d] area_size=%d, base_area_size=%d, diff=%d\n", num_pressed, area_size, base_area_size, ABS(base_area_size - area_size));
				ErrorF("[GroupTap][M] cx=%d, base_cx=%d, diff=%d\n", cx, base_cx, ABS(cx-base_cx));
				ErrorF("[GroupTap][M] cy=%d, base_cy=%d, diff=%d\n", cy, base_cy, ABS(cy-base_cy));
#endif//__DETAIL_DEBUG__

				if( ABS(base_area_size-area_size) >= TAP_AREA_THRESHOLD )
				{
#ifdef __DETAIL_DEBUG__
					ErrorF("[GroupTap][M] diff between area size(=%d) and base area size(=%d) is bigger than threshold(=%d)!\n", area_size, base_area_size, ABS(base_area_size-area_size));
#endif//__DETAIL_DEBUG__
					goto cleanup_tap;
				}

				if( !INBOX(&base_box_ext, cx, cy) )
				{
#ifdef __DETAIL_DEBUG__
					ErrorF("[GroupTap][M] current center coordinates is not in base coordinates box !\n");
#endif//__DETAIL_DEBUG__
					goto cleanup_tap;
				}
			}
			break;

		case ET_TouchEnd:
			if( g_pGesture->num_pressed )
				break;

			if( !tap_repeat )
			{
				prev_num_pressed = num_pressed;
			}

			prev_tap_repeat = tap_repeat;
			tap_repeat++;

#ifdef __DETAIL_DEBUG__
			ErrorF("[GroupTap][R] tap_repeat=%d, prev_tap_repeat=%d, num_pressed=%d, prev_num_pressed=%d\n", tap_repeat, prev_tap_repeat, num_pressed, prev_num_pressed);
#endif//__DETAIL_DEBUG__

			if( num_pressed != prev_num_pressed || !GestureHasFingerEventMask(GestureNotifyTap, num_pressed) )
			{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupTap][R] num_pressed(=%d) != prev_num_pressed(=%d) OR %d finger tap event was not grabbed/selected !\n",
					num_pressed, prev_num_pressed, num_pressed);
#endif//__DETAIL_DEBUG__
				goto cleanup_tap;
			}

			if( tap_repeat < MAX_TAP_REPEATS )
			{
				state = 2;
				TimerCancel(tap_event_timer);
				tap_event_timer = TimerSet(tap_event_timer, 0, DBL_TAP_TIME_THRESHOLD, GestureEventTimerHandler, (int *)&event_type);
				num_pressed = 0;
				break;
			}

#ifdef __DETAIL_DEBUG__
			ErrorF("[GroupTap][R] %d finger %s\n", num_pressed, (tap_repeat==2) ? "DBL_TAP" : "TRIPLE_TAP");
#endif//__DETAIL_DEBUG__

			if( GestureHasFingerEventMask(GestureNotifyTap, num_pressed) )
				GestureHandleGesture_Tap(num_pressed, tap_repeat, base_cx, base_cy);

			if( tap_repeat >= MAX_TAP_REPEATS )
			{
				goto cleanup_tap;
			}

			prev_num_pressed = num_pressed;
			num_pressed = 0;
			break;
	}

	return;

cleanup_tap:

	if( 0 == state )
		g_pGesture->recognized_gesture &= ~TapFilterMask;
	g_pGesture->filter_mask |= TapFilterMask;

	if( g_pGesture->filter_mask == GESTURE_FILTER_MASK_ALL )
	{
#ifdef __DETAIL_DEBUG__
		ErrorF("[GroupTap][cleanup] GestureFlushOrDrop() !\n");
#endif//__DETAIL_DEBUG__

		if( ERROR_INVALPTR == GestureFlushOrDrop() )
		{
			GestureControl(g_pGesture->this_device, DEVICE_OFF);
		}
	}

	num_pressed = 0;
	tap_repeat = 0;
	prev_num_pressed = 0;
	mbits = 0;
	base_time = 0;
	state = 0;
	TimerCancel(tap_event_timer);
	return;
}

void
GestureRecognize_GroupTapNHold(int type, InternalEvent *ev, DeviceIntPtr device, int idx, int timer_expired)
{
	static int num_pressed = 0;
	static int base_area_size = 0;
	static Time base_time = 0;
	static int base_cx;
	static int base_cy;
	int cx, cy;
	static pixman_box16_t base_box_ext;
	int area_size;
	static int mbits = 0;

	static int tap_repeat = 0;
	static int prev_num_pressed = 0;

	static OsTimerPtr tapnhold_event_timer = NULL;
	static int event_type = GestureNotifyTapNHold;
	static int state = GestureEnd;

	Time interval = 0;
	Time holdtime = 0;

	if( timer_expired )
	{
		if( (state == GestureEnd) && num_pressed )
		{
#ifdef __DETAIL_DEBUG__
			ErrorF("[GroupTapNHold][Timer][state=%d] Tap time expired !(num_pressed=%d, tap_repeat=%d)\n", GestureEnd, tap_repeat, num_pressed, tap_repeat);
#endif//__DETAIL_DEBUG__
			state = 0;
			goto cleanup_tapnhold;
		}

		if( state == GestureDone )
		{
#ifdef __DETAIL_DEBUG__
			ErrorF("[GroupTapNHold][Timer][state=%d] Interval between Tap and Hold is too long !\n");
#endif//__DETAIL_DEBUG__
			goto cleanup_tapnhold;
		}

#ifdef __DETAIL_DEBUG__
		switch( state )
		{
			case GestureBegin:
				ErrorF("[GroupTapNHold][Timer] TapNHold Begin !\n");
				break;

			case GestureUpdate:
				ErrorF("[GroupTapNHold][Timer] TapNHold Update !\n");
				break;
		}
#endif//__DETAIL_DEBUG__

		if( GestureHasFingerEventMask(GestureNotifyTapNHold, prev_num_pressed) )
		{
			GestureHandleGesture_TapNHold(prev_num_pressed, base_cx, base_cy, interval, holdtime, state);
			tapnhold_event_timer = TimerSet(tapnhold_event_timer, 0, TAPNHOLD_HOLD_TIME_THRESHOLD, GestureEventTimerHandler, (int *)&event_type);
		}
		else
		{
#ifdef __DETAIL_DEBUG__
			ErrorF("[GroupTapNHold][Timer] %d finger TapNHold event was not grabbed/selected !\n", prev_num_pressed);
#endif//__DETAIL_DEBUG__
			goto cleanup_tapnhold;
		}

		if( state <= GestureBegin )
			state++;
		return;
	}

	switch( type )
	{
		case ET_TouchBegin:
			g_pGesture->fingers[idx].flags |= PressFlagTapNHold;

			if( g_pGesture->num_pressed < 2 )
				return;

			//if( !prev_num_pressed && (!base_area_size || g_pGesture->num_pressed > num_pressed) )
			if( !base_area_size || g_pGesture->num_pressed > num_pressed )
			{

				if( state == GestureUpdate )
				{
#ifdef __DETAIL_DEBUG__
					ErrorF("[GroupTapNHold][P][cleanup] num_finger changed ! num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
					goto cleanup_tapnhold;
				}

				if( state == GestureDone )
					state = GestureBegin;

				base_area_size = AREA_SIZE(&g_pGesture->area.extents);
				base_cx = AREA_CENTER_X(&g_pGesture->area.extents);
				base_cy = AREA_CENTER_Y(&g_pGesture->area.extents);
				base_time = GetTimeInMillis();
				base_box_ext.x1 = base_cx-TAPNHOLD_MOVE_THRESHOLD;
				base_box_ext.y1 = base_cy-TAPNHOLD_MOVE_THRESHOLD;
				base_box_ext.x2 = base_cx+TAPNHOLD_MOVE_THRESHOLD;
				base_box_ext.y2 = base_cy+TAPNHOLD_MOVE_THRESHOLD;
				if( state == GestureEnd )
					tapnhold_event_timer = TimerSet(tapnhold_event_timer, 0, TAPNHOLD_TAP_TIME_THRESHOLD, GestureEventTimerHandler, (int *)&event_type);
				else
				{
					TimerCancel(tapnhold_event_timer);
					tapnhold_event_timer = TimerSet(tapnhold_event_timer, 0, TAPNHOLD_HOLD_TIME_THRESHOLD, GestureEventTimerHandler, (int *)&event_type);
				}
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupTapNHold][P] Create Timer !(state=%d)\n", state);
#endif//__DETAIL_DEBUG__
			}

			num_pressed = g_pGesture->num_pressed;

#ifdef __DETAIL_DEBUG__
			ErrorF("[GroupTapNHold][P][num_pressed=%d] AREA_SIZE(area.extents)=%d, base_cx=%d, base_cy=%d, base_time=%d\n", num_pressed, base_area_size, base_cx, base_cy, base_time);
#endif//__DETAIL_DEBUG__
			break;

		case ET_TouchUpdate:
			if( !(g_pGesture->fingers[idx].flags & PressFlagTapNHold ) )
				break;

			if( num_pressed < 2 )
				return;

			if( num_pressed != g_pGesture->num_pressed )
			{
				if( state != GestureEnd )
				{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupTapNHold][M][cleanup] num_finger changed ! num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
					goto cleanup_tapnhold;
				}
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupTapNHold][M][cleanup] num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
				//goto cleanup_tapnhold;
			}

			mbits |= (1 << idx);
			if( mbits == (pow(2, num_pressed)-1) )
			{
				area_size = AREA_SIZE(&g_pGesture->area.extents);
				cx = AREA_CENTER_X(&g_pGesture->area.extents);
				cy = AREA_CENTER_Y(&g_pGesture->area.extents);
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupTapNHold][M][num_pressed=%d] area_size=%d, base_area_size=%d, diff=%d\n", num_pressed, area_size, base_area_size, ABS(base_area_size - area_size));
				ErrorF("[GroupTapNHold][M] cx=%d, base_cx=%d, diff=%d\n", cx, base_cx, ABS(cx-base_cx));
				ErrorF("[GroupTapNHold][M] cy=%d, base_cy=%d, diff=%d\n", cy, base_cy, ABS(cy-base_cy));
#endif//__DETAIL_DEBUG__

				if( ABS(base_area_size-area_size) >= TAPNHOLD_AREA_THRESHOLD )
				{
#ifdef __DETAIL_DEBUG__
					ErrorF("[GroupTapNHold][M] diff between area size(=%d) and base area size(=%d) is bigger than threshold(=%d)!\n", area_size, base_area_size, ABS(base_area_size-area_size));
#endif//__DETAIL_DEBUG__
					goto cleanup_tapnhold;
				}

				if( !INBOX(&base_box_ext, cx, cy) )
				{
#ifdef __DETAIL_DEBUG__
					ErrorF("[GroupTapNHold][M] current center coordinates is not in base coordinates box !\n");
#endif//__DETAIL_DEBUG__
					goto cleanup_tapnhold;
				}
			}
			break;

		case ET_TouchEnd:
			if( state != GestureEnd && num_pressed >= 2)
			{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupTapNHold][R][cleanup] num_finger changed ! num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
				goto cleanup_tapnhold;
			}

			if( g_pGesture->num_pressed )
				break;

			if( !tap_repeat )
			{
				prev_num_pressed = num_pressed;
			}

			tap_repeat++;

#ifdef __DETAIL_DEBUG__
			ErrorF("[GroupTapNHold][R] tap_repeat=%d, num_pressed=%d, prev_num_pressed=%d\n", tap_repeat, num_pressed, prev_num_pressed);
#endif//__DETAIL_DEBUG__

			if( num_pressed != prev_num_pressed || !GestureHasFingerEventMask(GestureNotifyTapNHold, num_pressed) )
			{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupTapNHold][R] num_pressed(=%d) != prev_num_pressed(=%d) OR %d finger tap event was not grabbed/selected !\n",
					num_pressed, prev_num_pressed, num_pressed);
#endif//__DETAIL_DEBUG__
				goto cleanup_tapnhold;
			}

			if( tap_repeat > 1 )
			{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupTapNHold][R] Tap events(tap_repeat=%d) were put twice or more !(ignored)\n", tap_repeat);
#endif//__DETAIL_DEBUG__
				goto cleanup_tapnhold;
			}

			prev_num_pressed = num_pressed;
			num_pressed = 0;
			state = GestureDone;

			TimerCancel(tapnhold_event_timer);
			tapnhold_event_timer = TimerSet(tapnhold_event_timer, 0, TAPNHOLD_INTV_TIME_THRESHOLD, GestureEventTimerHandler, (int *)&event_type);

#ifdef __DETAIL_DEBUG__
			ErrorF("[GroupTapNHold][R][Last] state=%d, tap_repeat=%d, num_pressed=%d, prev_num_pressed=%d\n", state,  tap_repeat, num_pressed, prev_num_pressed);
#endif//__DETAIL_DEBUG__
			break;
	}

	return;

cleanup_tapnhold:

	if( state == GestureUpdate )
	{
		state = GestureEnd;
		if( GestureHasFingerEventMask(GestureNotifyTapNHold, prev_num_pressed) )
		{
			GestureHandleGesture_TapNHold(prev_num_pressed, base_cx, base_cy, interval, holdtime, state);
		}
	}
	else
	{
		g_pGesture->recognized_gesture &= ~TapNHoldFilterMask;
	}

	g_pGesture->filter_mask |= TapNHoldFilterMask;
	if( g_pGesture->filter_mask == GESTURE_FILTER_MASK_ALL )
	{
#ifdef __DETAIL_DEBUG__
		ErrorF("[GroupTapNHold][cleanup] GestureFlushOrDrop() !\n");
#endif//__DETAIL_DEBUG__

		if( ERROR_INVALPTR == GestureFlushOrDrop() )
		{
			GestureControl(g_pGesture->this_device, DEVICE_OFF);
		}
	}

	TimerCancel(tapnhold_event_timer);
	num_pressed = 0;
	tap_repeat = 0;
	prev_num_pressed = 0;
	mbits = 0;
	base_time = 0;
	state = 0;

	return;
}

void GestureRecognize_GroupHold(int type, InternalEvent *ev, DeviceIntPtr device, int idx, int timer_expired)
{
	static int num_pressed = 0;
	static int base_area_size = 0;
	static Time base_time = 0;
	static int base_cx;
	static int base_cy;
	int cx, cy;
	static pixman_box16_t base_box_ext;
	int area_size;
	static int state = GestureEnd;

	static OsTimerPtr hold_event_timer = NULL;
	static int event_type = GestureNotifyHold;

	if( timer_expired )
	{
		if( state <= GestureBegin )
			state++;

#ifdef __DETAIL_DEBUG__
		switch( state )
		{
			case GestureBegin:
				ErrorF("[GroupHold] HOLD Begin !\n");
				break;

			case GestureUpdate:
				ErrorF("[GroupHold] HOLD Update !\n");
				break;
		}
#endif//__DETAIL_DEBUG__

		if( GestureHasFingerEventMask(GestureNotifyHold, num_pressed) )
		{
			GestureHandleGesture_Hold(num_pressed, base_cx, base_cy, GetTimeInMillis()-base_time, state);
			hold_event_timer = TimerSet(hold_event_timer, 0, HOLD_TIME_THRESHOLD, GestureEventTimerHandler, (int *)&event_type);
		}
		return;
	}

	switch( type )
	{
		case ET_TouchBegin:
			g_pGesture->fingers[idx].flags |= PressFlagHold;

			if( g_pGesture->num_pressed < 2 )
				return;

			if( !base_area_size || g_pGesture->num_pressed > num_pressed )
			{
				if( state != GestureEnd )
				{
#ifdef __DETAIL_DEBUG__
					ErrorF("[GroupHold][P][cleanup] num_finger changed ! num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
					goto cleanup_hold;
				}

				base_area_size = AREA_SIZE(&g_pGesture->area.extents);
				base_cx = AREA_CENTER_X(&g_pGesture->area.extents);
				base_cy = AREA_CENTER_Y(&g_pGesture->area.extents);
				base_time = GetTimeInMillis();
				base_box_ext.x1 = base_cx-HOLD_MOVE_THRESHOLD;
				base_box_ext.y1 = base_cy-HOLD_MOVE_THRESHOLD;
				base_box_ext.x2 = base_cx+HOLD_MOVE_THRESHOLD;
				base_box_ext.y2 = base_cy+HOLD_MOVE_THRESHOLD;
				event_type = GestureNotifyHold;
				hold_event_timer = TimerSet(hold_event_timer, 0, HOLD_TIME_THRESHOLD, GestureEventTimerHandler, (int *)&event_type);
			}
			num_pressed = g_pGesture->num_pressed;

#ifdef __DETAIL_DEBUG__
			ErrorF("[GroupHold][P]][num_pressed=%d] AREA_SIZE(area.extents)=%d, base_cx=%d, base_cy=%d\n", num_pressed, base_area_size, base_cx, base_cy);
#endif//__DETAIL_DEBUG__
			break;

		case ET_TouchUpdate:
			if( !(g_pGesture->fingers[idx].flags & PressFlagHold ) )
				break;

			if( num_pressed < 2 )
				return;

			if( num_pressed != g_pGesture->num_pressed )
			{
				if( state != GestureEnd )
				{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupHold][M][cleanup] num_finger changed ! num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
					goto cleanup_hold;
				}
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupHold][M][cleanup] num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
				//goto cleanup_hold;
			}

			area_size = AREA_SIZE(&g_pGesture->area.extents);
			cx = AREA_CENTER_X(&g_pGesture->area.extents);
			cy = AREA_CENTER_Y(&g_pGesture->area.extents);
#ifdef __DETAIL_DEBUG__
			ErrorF("[GroupHold][M][num_pressed=%d] area_size=%d, base_area_size=%d, diff=%d\n", num_pressed, area_size, base_area_size, ABS(base_area_size - area_size));
			ErrorF("[GroupHold][M] cx=%d, base_cx=%d, diff=%d\n", cx, base_cx, ABS(cx-base_cx));
			ErrorF("[GroupHold][M] cy=%d, base_cy=%d, diff=%d\n", cy, base_cy, ABS(cy-base_cy));
#endif//__DETAIL_DEBUG__

			if( ABS(base_area_size-area_size) >= HOLD_AREA_THRESHOLD )
			{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupHold][M] diff between area size(=%d) and base area size(=%d) is bigger than threshold(=%d)!\n", area_size, base_area_size, ABS(base_area_size-area_size));
#endif//__DETAIL_DEBUG__
				goto cleanup_hold;
			}

			if( !INBOX(&base_box_ext, cx, cy) )
			{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupHold][M] current center coordinates is not in base coordinates box !\n");
#endif//__DETAIL_DEBUG__
				goto cleanup_hold;
			}
			break;

		case ET_TouchEnd:
			if( state != GestureEnd && num_pressed >= 2)
			{
#ifdef __DETAIL_DEBUG__
				ErrorF("[GroupHold][R][cleanup] num_finger changed ! num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
				goto cleanup_hold;
			}

			//ErrorF("[GroupHold][R] num_pressed=%d, g_pGesture->num_pressed=%d\n", num_pressed, g_pGesture->num_pressed);
			if( g_pGesture->num_pressed )
				break;

			goto cleanup_hold;
			break;
	}

	return;

cleanup_hold:

	if( state == GestureBegin || state == GestureUpdate )
	{
		state = GestureEnd;
		if( GestureHasFingerEventMask(GestureNotifyHold, num_pressed) )
		{
			GestureHandleGesture_Hold(num_pressed, base_cx, base_cy, GetTimeInMillis()-base_time, state);
		}
	}
	else
	{
		g_pGesture->recognized_gesture &= ~HoldFilterMask;
	}

	g_pGesture->filter_mask |= HoldFilterMask;
	num_pressed = 0;
	base_area_size = 0;
	base_time = 0;
	base_cx = base_cy = 0;
	state = GestureEnd;
	base_box_ext.x1 = base_box_ext.x2 = base_box_ext.y1 = base_box_ext.y2 = 0;
	TimerCancel(hold_event_timer);
	return;
}

static inline void
GestureEnableDisable()
{
	if((g_pGesture->grabMask) || (g_pGesture->lastSelectedWin != None))
	{
		GestureEnable(1, FALSE, g_pGesture->this_device);
	}
	else
	{
		GestureEnable(0, FALSE, g_pGesture->this_device);
	}
}

void
GestureCbEventsGrabbed(Mask *pGrabMask, GestureGrabEventPtr *pGrabEvent)
{
	g_pGesture->grabMask = *pGrabMask;
	g_pGesture->GrabEvents = pGrabEvent;
	GestureEnableDisable();
}

void
GestureCbEventsSelected(Window win, Mask *pEventMask)
{
	g_pGesture->lastSelectedWin = win;
	g_pGesture->lastSelectedMask = (pEventMask) ? *pEventMask : 0;
	GestureEnableDisable();
}

WindowPtr
GestureGetEventsWindow(void)
{
	Mask mask;
	WindowPtr pWin;

	pWin = GestureWindowOnXY(g_pGesture->fingers[0].px, g_pGesture->fingers[0].py);

	if( pWin )
	{
#ifdef __DETAIL_DEBUG__
		ErrorF("[X11][GestureGetEventsWindow] pWin->drawable.id=0x%x\n", pWin->drawable.id);
#endif//__DETAIL_DEBUG__
		g_pGesture->gestureWin = pWin->drawable.id;
	}
	else
	{
#ifdef __DETAIL_DEBUG__
		ErrorF("[X11][GestureGetEventsWindow] GestureWindowOnXY returns NULL !\n");
#endif//__DETAIL_DEBUG__
		return NULL;
	}

	if(g_pGesture->gestureWin == g_pGesture->lastSelectedWin)
	{
		g_pGesture->eventMask = g_pGesture->lastSelectedMask;
		goto nonempty_eventmask;
	}

	//check selected event(s)
	if( !GestureHasSelectedEvents(pWin, &g_pGesture->eventMask) )
	{
		g_pGesture->eventMask = 0;
	}
	else
	{
		g_pGesture->lastSelectedWin = g_pGesture->gestureWin;
		g_pGesture->lastSelectedMask = g_pGesture->eventMask;
	}

	if((!g_pGesture->eventMask) && (!g_pGesture->grabMask))
	{
#ifdef __DETAIL_DEBUG__
		ErrorF("[X11][GestureGetEventsWindow] No grabbed events or no events were selected for window(0x%x) !\n", pWin->drawable.id);
#endif//__DETAIL_DEBUG__
		return NULL;
	}

nonempty_eventmask:

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureGetEventsWindow] g_pGesture->eventMask=0x%x\n", g_pGesture->eventMask);
#endif//__DETAIL_DEBUG__

	mask = (GESTURE_FILTER_MASK_ALL & ~(g_pGesture->grabMask | g_pGesture->eventMask));

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureGetEventsWindow] g_pGesture->filter_mask=0x%x, mask=0x%x\n", g_pGesture->filter_mask, mask);
#endif//__DETAIL_DEBUG__

	g_pGesture->filter_mask = mask;

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureGetEventsWindow] g_pGesture->filter_mask=0x%x\n", g_pGesture->filter_mask);
#endif//__DETAIL_DEBUG__

	return pWin;
}

static CARD32
GestureEventTimerHandler(OsTimerPtr timer, CARD32 time, pointer arg)
{
	int event_type = *(int *)arg;

	switch( event_type )
	{
		case GestureNotifyHold:
#ifdef __DETAIL_DEBUG__
			ErrorF("[GestureEventTimerHandler] GestureNotifyHold (event_type = %d)\n", event_type);
#endif//__DETAIL_DEBUG__
			GestureRecognize_GroupHold(event_type, NULL, NULL, 0, 1);
			break;

		case GestureNotifyPan:
#ifdef __DETAIL_DEBUG__
			ErrorF("[GestureEventTimerHandler] GestureNotifyPan (event_type = %d)\n", event_type);
#endif//__DETAIL_DEBUG__
			GestureRecognize_GroupPan(event_type, NULL, NULL, 0, 1);
			break;

		case GestureNotifyTap:
#ifdef __DETAIL_DEBUG__
			ErrorF("[GestureEventTimerHandler] GestureNotifyTap (event_type = %d)\n", event_type);
#endif//__DETAIL_DEBUG__
			GestureRecognize_GroupTap(event_type, NULL, NULL, 0, 1);
			break;

		case GestureNotifyTapNHold:
#ifdef __DETAIL_DEBUG__
			ErrorF("[GestureEventTimerHandler] GestureNotifyTapNHold (event_type = %d)\n", event_type);
#endif//__DETAIL_DEBUG__
			GestureRecognize_GroupTapNHold(event_type, NULL, NULL, 0, 1);
			break;

		case GestureNotifyPinchRotation:
#ifdef __DETAIL_DEBUG__
			ErrorF("[GestureEventTimerHandler] GestureNotifyPinchRotation (event_type = %d)\n", event_type);
#endif//__DETAIL_DEBUG__
			GestureRecognize_GroupPinchRotation(event_type, NULL, NULL, 0, 1);
			break;

		default:
#ifdef __DETAIL_DEBUG__
			ErrorF("[GestureEventTimerHandler] unknown event_type (=%d)\n", event_type);
#endif//__DETAIL_DEBUG__
			if(timer)
				ErrorF("[GestureEventTimerHandler] timer=%x\n", (unsigned int)timer);
	}

	return 0;
}

static CARD32
GestureSingleFingerTimerHandler(OsTimerPtr timer, CARD32 time, pointer arg)
{
	g_pGesture->filter_mask = GESTURE_FILTER_MASK_ALL;
	g_pGesture->recognized_gesture = 0;

	if( ERROR_INVALPTR == GestureFlushOrDrop() )
	{
		GestureControl(g_pGesture->this_device, DEVICE_OFF);
	}

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][Single] expired !\n");
#endif//__DETAIL_DEBUG__

	return 0;
}

static uint32_t
GestureTouchFindDDXIDByTouchID(DeviceIntPtr device, uint32_t touchid)
{
	int i;
	DDXTouchPointInfoPtr ti;

	for( i = 0 ; i < g_pGesture->num_touches ; i++ )
	{
#ifdef __DETAIL_DEBUG__
		ErrorF("[X11][GestureTouchFindDDXIDByTouchID][check existence] fingers[%d].touchid=%d, touchid=%d\n", i, g_pGesture->fingers[i].touchid, touchid);
#endif//__DETAIL_DEBUG__

		if(g_pGesture->fingers[i].touchid == touchid)
		{
#ifdef __DETAIL_DEBUG__
			ErrorF("[X11][GestureTouchFindDDXIDByTouchID][exist] ddx_id=%d was found !\n", i);
#endif//__DETAIL_DEBUG__

			return (uint32_t)i;
		}
	}

	for( i = 0 ; i < device->last.num_touches ; i++)
	{
		ti = &device->last.touches[i];

#ifdef __DETAIL_DEBUG__
		ErrorF("[X11][GestureTouchFindDDXIDByTouchID][alloc new] i=%d, ti->client_id=%d, touchid=%d, ti->ddx_id=%d, ti->active=%d\n", i, ti->client_id, touchid, ti->ddx_id, ti->active);
#endif//__DETAIL_DEBUG__

		if(ti->client_id == touchid)
		{
#ifdef __DETAIL_DEBUG__
			ErrorF("[X11][GestureTouchFindDDXIDByTouchID][new] ddx_id=%d was found !\n", ti->ddx_id);
#endif//__DETAIL_DEBUG__

			g_pGesture->fingers[ti->ddx_id].touchid = touchid;
			return ti->ddx_id;
		}
	}

	return (uint32_t)-1;
}

void
GestureRecognize(int type, InternalEvent *ev, DeviceIntPtr device, int idx)
{
	int i;
	static OsTimerPtr single_finger_timer = NULL;

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureRecognize] ev->any.type=%d, type=%d, device->id=%d\n", ev->any.type, type, device->id);
#endif//__DETAIL_DEBUG__

	if((PROPAGATE_EVENTS == g_pGesture->ehtype) || (device->id != g_pGesture->first_fingerid))
		return;

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureRecognize] idx=%d\n", idx);
#endif//__DETAIL_DEBUG__

	switch( type )
	{
		case ET_TouchBegin:
			if( idx == 0 )
				g_pGesture->event_sum[0] = BTN_PRESSED;
			g_pGesture->fingers[idx].ptime = ev->any.time;
			g_pGesture->fingers[idx].px = ev->device_event.root_x;
			g_pGesture->fingers[idx].py = ev->device_event.root_y;

			g_pGesture->num_pressed++;
			if( g_pGesture->num_pressed == 1 )
			{
				single_finger_timer = TimerSet(single_finger_timer, 0, 50, GestureSingleFingerTimerHandler, NULL);
			}
			else
			{
				TimerCancel(single_finger_timer);
			}

			if( g_pGesture->num_pressed > g_pGesture->num_touches )
				g_pGesture->num_pressed = g_pGesture->num_touches;

			if( !g_pGesture->pTempWin || g_pGesture->num_pressed != g_pGesture->inc_num_pressed )
			{
				g_pGesture->pTempWin = GestureGetEventsWindow();

				if( NULL == g_pGesture->pTempWin )
				{
#ifdef __DETAIL_DEBUG__
					ErrorF("[X11][GestureRecognize][g_pGesture->num_pressed=%d] No events were selected !\n", g_pGesture->num_pressed);
#endif//__DETAIL_DEBUG__
					g_pGesture->filter_mask = GESTURE_FILTER_MASK_ALL;
					goto flush_or_drop;
				}
			}

			g_pGesture->inc_num_pressed = g_pGesture->num_pressed;

			g_pGesture->finger_rects[idx].extents.x1 = ev->device_event.root_x - FINGER_WIDTH;
			g_pGesture->finger_rects[idx].extents.x2 = ev->device_event.root_x + FINGER_WIDTH;
			g_pGesture->finger_rects[idx].extents.y1 =  ev->device_event.root_y - FINGER_HEIGHT;
			g_pGesture->finger_rects[idx].extents.y2 =  ev->device_event.root_y + FINGER_HEIGHT;

			if( g_pGesture->inc_num_pressed == 1 )
			{
				pixman_region_union(&g_pGesture->area, &g_pGesture->finger_rects[0], &g_pGesture->finger_rects[0]);
#ifdef __DETAIL_DEBUG__
				ErrorF("[P][g_pGesture->inc_num_pressed=1] AREA_SIZE(area.extents)=%d\n", AREA_SIZE(&g_pGesture->area.extents));
#endif//__DETAIL_DEBUG__
			}
			else
			{
				pixman_region_union(&g_pGesture->area, &g_pGesture->finger_rects[0], &g_pGesture->finger_rects[0]);
				for( i = 1 ; i < g_pGesture->inc_num_pressed ; i++ )
				{
					pixman_region_union(&g_pGesture->area, &g_pGesture->area, &g_pGesture->finger_rects[i]);
				}
#ifdef __DETAIL_DEBUG__
				ErrorF("[P][g_pGesture->inc_num_pressed=%d] AREA_SIZE(area.extents)=%d\n", g_pGesture->inc_num_pressed, AREA_SIZE(&g_pGesture->area.extents));
#endif//__DETAIL_DEBUG__
			}
			break;

		case ET_TouchUpdate:
			if( !g_pGesture->fingers[idx].ptime )
				return;

			g_pGesture->fingers[idx].mx = ev->device_event.root_x;
			g_pGesture->fingers[idx].my = ev->device_event.root_y;

			if( idx == 0 && g_pGesture->event_sum[0] )
			{
				g_pGesture->event_sum[0] += BTN_MOVING;
#ifdef __DETAIL_DEBUG__
				ErrorF("[X11][GestureRecognize] moving !\n");
#endif//__DETAIL_DEBUG__
				if( g_pGesture->event_sum[0] >= 7 )
				{
                               if( (g_pGesture->event_sum[0] >= 7) && (g_pGesture->inc_num_pressed < 2) )
                                {
 #ifdef __DETAIL_DEBUG__
                                       ErrorF("[X11][GestureRecognize] moving limit!\n");
 #endif//__DETAIL_DEBUG__
                                       g_pGesture->filter_mask = GESTURE_FILTER_MASK_ALL;
					    goto flush_or_drop;
                                }
				}
			}

			g_pGesture->finger_rects[idx].extents.x1 = ev->device_event.root_x - FINGER_WIDTH;
			g_pGesture->finger_rects[idx].extents.x2 = ev->device_event.root_x + FINGER_WIDTH;
			g_pGesture->finger_rects[idx].extents.y1 =  ev->device_event.root_y - FINGER_HEIGHT;
			g_pGesture->finger_rects[idx].extents.y2 =  ev->device_event.root_y + FINGER_HEIGHT;

			if( g_pGesture->inc_num_pressed == 1 )
			{
				pixman_region_union(&g_pGesture->area, &g_pGesture->finger_rects[0], &g_pGesture->finger_rects[0]);
#ifdef __DETAIL_DEBUG__
				ErrorF("[M][g_pGesture->inc_num_pressed=1] AREA_SIZE(area)=%d\n", AREA_SIZE(&g_pGesture->area.extents));
#endif//__DETAIL_DEBUG__
			}
			else
			{
				pixman_region_union(&g_pGesture->area, &g_pGesture->finger_rects[0], &g_pGesture->finger_rects[0]);
				for( i = 1 ; i < g_pGesture->inc_num_pressed ; i++ )
				{
					pixman_region_union(&g_pGesture->area, &g_pGesture->area, &g_pGesture->finger_rects[i]);
				}
#ifdef __DETAIL_DEBUG__
				ErrorF("[M][g_pGesture->inc_num_pressed=%d] AREA_SIZE(area)=%d\n", g_pGesture->inc_num_pressed, AREA_SIZE(&g_pGesture->area.extents));
#endif//__DETAIL_DEBUG__
			}
			break;

		case ET_TouchEnd:
			g_pGesture->fingers[idx].rtime = ev->any.time;
			g_pGesture->fingers[idx].rx = ev->device_event.root_x;
			g_pGesture->fingers[idx].ry = ev->device_event.root_y;

			g_pGesture->num_pressed--;
			if( g_pGesture->num_pressed <= 0 )
			{
#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureRecognize] All fingers were released !\n");
#endif//__DETAIL_DEBUG__
				if( g_pGesture->inc_num_pressed == 1 )
					goto flush_or_drop;
			}
			break;
	}

	if( g_pGesture->filter_mask != GESTURE_FILTER_MASK_ALL )
	{
		if( !(g_pGesture->filter_mask & FlickFilterMask) )
		{
			GestureRecognize_GroupFlick(type, ev, device, idx);
		}
		if( !(g_pGesture->filter_mask & PanFilterMask) )
		{
			GestureRecognize_GroupPan(type, ev, device, idx, 0);
		}
		if( !(g_pGesture->filter_mask & PinchRotationFilterMask) )
		{
			GestureRecognize_GroupPinchRotation(type, ev, device, idx, 0);
		}
		if( !(g_pGesture->filter_mask & TapFilterMask) )
		{
			GestureRecognize_GroupTap(type, ev, device, idx, 0);
		}
		if( !(g_pGesture->filter_mask & TapNHoldFilterMask) )
		{
			GestureRecognize_GroupTapNHold(type, ev, device, idx, 0);
		}
		if( !(g_pGesture->filter_mask & HoldFilterMask) )
		{
			GestureRecognize_GroupHold(type, ev, device, idx, 0);
		}
	}

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureRecognize][N] g_pGesture->filter_mask = 0x%x\n", g_pGesture->filter_mask);
	ErrorF("[X11][GestureRecognize][N] g_pGesture->GESTURE_FILTER_MASK_ALL = 0x%x\n", GESTURE_FILTER_MASK_ALL);
	ErrorF("[X11][GestureRecognize][N] g_pGesture->recognized_gesture=0x%x\n", g_pGesture->recognized_gesture);
#endif//__DETAIL_DEBUG__

	if( g_pGesture->filter_mask == GESTURE_FILTER_MASK_ALL )
	{
		if( !g_pGesture->recognized_gesture )
			goto flush_or_drop;
		else if( !g_pGesture->num_pressed )
			goto flush_or_drop;
 	}

	if( g_pGesture->recognized_gesture )
	{
		if( g_pGesture->ehtype == KEEP_EVENTS )
			GestureEventsDrop();
		g_pGesture->ehtype = IGNORE_EVENTS;
	}

	return;

flush_or_drop:

#ifdef __DETAIL_DEBUG__
	ErrorF("[GestureRecognize] GestureFlushOrDrop() !\n");
#endif//__DETAIL_DEBUG__
	if( ERROR_INVALPTR == GestureFlushOrDrop() )
	{
		GestureControl(g_pGesture->this_device, DEVICE_OFF);
	}
}


ErrorStatus GestureFlushOrDrop(void)
{
	ErrorStatus err;

	if( g_pGesture->recognized_gesture )
	{
		GestureEventsDrop();
	}
	else
	{
		g_pGesture->ehtype = PROPAGATE_EVENTS;

		err = GestureEventsFlush();
		if( ERROR_NONE != err )
			return err;

#ifdef __DETAIL_DEBUG__
		ErrorF("[X11][GestureFlushOrDrop][F] g_pGesture->filter_mask = 0x%x\n", g_pGesture->filter_mask);
		ErrorF("[X11][GestureFlushOrDrop][F] g_pGesture->GESTURE_FILTER_MASK_ALL = 0x%x\n", GESTURE_FILTER_MASK_ALL);
		ErrorF("[X11][GestureFlushOrDrop][F] g_pGesture->recognized_gesture=0x%x\n", g_pGesture->recognized_gesture);
#endif//__DETAIL_DEBUG__
	}

	err = GestureRegionsReinit();
	if( ERROR_NONE != err )
		return err;

	g_pGesture->pTempWin = NULL;
	g_pGesture->inc_num_pressed = g_pGesture->num_pressed = 0;
	g_pGesture->event_sum[0] = 0;

	return ERROR_NONE;
}

void
GestureHandleMTSyncEvent(int screen_num, InternalEvent *ev, DeviceIntPtr device)
{
	int i;

#ifdef __DEBUG_EVENT_HANDLER__
	ErrorF("\n[X11][GestureHandleMTSyncEvent] (%d:%d) time:%d cur:%d\n",
			ev->any_event.deviceid, ev->any_event.sync, (int)ev->any.time, (int)GetTimeInMillis());
#endif//__DEBUG_EVENT_HANDLER__

	if( MTOUCH_FRAME_SYNC_BEGIN == ev->any_event.sync )
	{
		g_pGesture->ehtype = KEEP_EVENTS;
		g_pGesture->filter_mask = 0;
		g_pGesture->recognized_gesture = 0;
		g_pGesture->num_pressed = 0;

		for( i=0 ; i < g_pGesture->num_touches ; i++ )
		{
			g_pGesture->fingers[i].ptime = 0;
			g_pGesture->fingers[i].touchid = -1;
		}
	}
	else if( MTOUCH_FRAME_SYNC_END == ev->any_event.sync )
	{
		g_pGesture->ehtype = PROPAGATE_EVENTS;
	}
}

void GestureHandleTouchBeginEvent(int screen_num, InternalEvent *ev, DeviceIntPtr device)
{
	int idx = -1;
	int touchid;
#ifdef __DEBUG_EVENT_HANDLER__
	ErrorF("[X11][GestureHandleTouchBeginEvent] devid=%d, touchid=%d, time:%d cur:%d\n", device->id, ev->device_event.touchid, ev->any.time, GetTimeInMillis());
#endif//__DEBUG_EVENT_HANDLER__

	switch( g_pGesture->ehtype )
	{
		case KEEP_EVENTS:
			if( !g_pGesture->num_touches )
			{
				device->public.processInputProc(ev, device);
				return;
			}

			if( ERROR_INVALPTR == GestureEnqueueEvent(screen_num, ev,  device) )
			{
				GestureControl(g_pGesture->this_device, DEVICE_OFF);
				return;
			}

			if(device->id != g_pGesture->first_fingerid)
				return;

			touchid = ev->device_event.touchid;
			idx = GestureTouchFindDDXIDByTouchID(device, touchid);

			if( idx >= 0 )
				GestureRecognize(ET_TouchBegin, ev, device, idx);
			break;

		case PROPAGATE_EVENTS:
			device->public.processInputProc(ev, device);
			break;

		case IGNORE_EVENTS:
			touchid = ev->device_event.touchid;
			idx = GestureTouchFindDDXIDByTouchID(device, touchid);

			if( idx >= 0 )
				GestureRecognize(ET_TouchBegin, ev, device, idx);
			break;

		default:
			break;
	}
}

void GestureHandleTouchUpdateEvent(int screen_num, InternalEvent *ev, DeviceIntPtr device)
{
	int idx = -1;
	int touchid;
#ifdef __DEBUG_EVENT_HANDLER__
	ErrorF("[X11][GestureHandleTouchUpdateEvent] devid=%d, touchid=%d, time:%d cur:%d\n", device->id, ev->device_event.touchid, ev->any.time, GetTimeInMillis());
#endif//__DEBUG_EVENT_HANDLER__

	switch( g_pGesture->ehtype )
	{
		case KEEP_EVENTS:
			if( !g_pGesture->num_touches )
			{
				device->public.processInputProc(ev, device);
				return;
			}

			if( ERROR_INVALPTR == GestureEnqueueEvent(screen_num, ev,  device) )
			{
				GestureControl(g_pGesture->this_device, DEVICE_OFF);
				return;
			}

			if(device->id != g_pGesture->first_fingerid)
				return;

			touchid = ev->device_event.touchid;
			idx = GestureTouchFindDDXIDByTouchID(device, touchid);

			if( idx >= 0 )
				GestureRecognize(ET_TouchUpdate, ev, device, idx);
			break;

		case PROPAGATE_EVENTS:
			device->public.processInputProc(ev, device);
			break;

		case IGNORE_EVENTS:
			touchid = ev->device_event.touchid;
			idx = GestureTouchFindDDXIDByTouchID(device, touchid);

			if( idx >= 0 )
				GestureRecognize(ET_TouchUpdate, ev, device, idx);
			break;

		default:
			break;
	}
}

void GestureHandleTouchEndEvent(int screen_num, InternalEvent *ev, DeviceIntPtr device)
{
	int idx = -1;
	int touchid;
#ifdef __DEBUG_EVENT_HANDLER__
	ErrorF("[X11][GestureHandleTouchEndEvent] devid=%d, touchid=%d, time:%d cur:%d\n", device->id, ev->device_event.touchid, ev->any.time, GetTimeInMillis());
#endif//__DEBUG_EVENT_HANDLER__

	switch( g_pGesture->ehtype )
	{
		case KEEP_EVENTS:
			if( !g_pGesture->num_touches )
			{
				device->public.processInputProc(ev, device);
				return;
			}

			if( ERROR_INVALPTR == GestureEnqueueEvent(screen_num, ev,  device) )
			{
				GestureControl(g_pGesture->this_device, DEVICE_OFF);
				return;
			}

			if(device->id != g_pGesture->first_fingerid)
				return;

			touchid = ev->device_event.touchid;
			idx = GestureTouchFindDDXIDByTouchID(device, touchid);

			if( idx >= 0 )
			{
				GestureRecognize(ET_TouchEnd, ev, device, idx);
				g_pGesture->fingers[idx].touchid = -1;
			}
			break;

		case PROPAGATE_EVENTS:
			device->public.processInputProc(ev, device);
			break;

		case IGNORE_EVENTS:
			touchid = ev->device_event.touchid;
			idx = GestureTouchFindDDXIDByTouchID(device, touchid);

			if( idx >= 0 )
				GestureRecognize(ET_TouchEnd, ev, device, idx);
			break;

		default:
			break;
	}
}

static ErrorStatus
GestureEnableEventHandler(InputInfoPtr pInfo)
 {
 	Bool res;
	GestureDevicePtr pGesture = pInfo->private;

	res = GestureInstallResourceStateHooks();

	if( !res )
	{
		ErrorF("[X11][GestureEnableEventHandler] Failed on GestureInstallResourceStateHooks() !\n");
		return ERROR_ABNORMAL;
	}

	res = GestureRegisterCallbacks(GestureCbEventsGrabbed, GestureCbEventsSelected);

	if( !res )
	{
		ErrorF("[X11][GestureEnableEventHandler] Failed to register callbacks for GestureEventsGrabbed(), GestureEventsSelected() !\n");
		goto failed;
	}

	pGesture->device_setting_timer = TimerSet(pGesture->device_setting_timer, 0, 5000, GestureTimerHandler, pInfo);

	if( !pGesture->device_setting_timer )
	{
		ErrorF("[X11][GestureEnableEventHandler] Failed to allocate memory for timer !\n");
		goto failed;
	}

	return ERROR_NONE;

failed:
	GestureUninstallResourceStateHooks();
	GestureUnsetMaxNumberOfFingers();

	return ERROR_ABNORMAL;
}

static ErrorStatus
GestureDisableEventHandler(void)
{
	ErrorStatus err = ERROR_NONE;

	mieqSetHandler(ET_TouchBegin, NULL);
	mieqSetHandler(ET_TouchUpdate, NULL);
	mieqSetHandler(ET_TouchEnd, NULL);
	mieqSetHandler(ET_MTSync, NULL);

	err = GestureFiniEQ();

	if( ERROR_INVALPTR == err )
	{
		ErrorF("[X11][GestureDisableEventHandler] EQ is invalid or was freed already !\n");
	}

	GestureRegisterCallbacks(NULL, NULL);
	GestureUninstallResourceStateHooks();

	return err;
}

static CARD32
GestureTimerHandler(OsTimerPtr timer, CARD32 time, pointer arg)
{
	InputInfoPtr pInfo = (InputInfoPtr)arg;
	GestureDevicePtr pGesture = pInfo->private;

	Bool res;
	DeviceIntPtr dev;
	for( dev = inputInfo.pointer ; dev; dev = dev->next )
	{
		if(IsMaster(dev) && IsPointerDevice(dev))
		{
			pGesture->master_pointer = dev;
			ErrorF("[X11][GestureTimerHandler][id:%d] Master Pointer=%s\n", dev->id, pGesture->master_pointer->name);
			continue;
		}

		if(IsXTestDevice(dev, NULL) && IsPointerDevice(dev))
		{
			pGesture->xtest_pointer = dev;
			ErrorF("[X11][GestureTimerHandler][id:%d] XTest Pointer=%s\n", dev->id, pGesture->xtest_pointer->name);
			continue;
		}

		if(IsPointerDevice(dev) && dev->touch)
		{
			pGesture->num_touches = dev->touch->num_touches + 1;
			pGesture->first_fingerid = dev->id;
#ifdef __DETAIL_DEBUG__
			ErrorF("[X11][GestureTimerHandler] num_touches = %d, first_fingerid=%d\n", pGesture->num_touches, pGesture->first_fingerid);
#endif//__DETAIL_DEBUG__
		}
	}

	if( !pGesture->master_pointer || !pGesture->xtest_pointer )
	{
		ErrorF("[X11][GestureTimerHandler] Failed to get info of master pointer or XTest pointer !\n");
		pGesture->device_setting_timer = TimerSet(pGesture->device_setting_timer, 0, 0, NULL, NULL);
		pGesture->num_touches = 0;

		return 0;
	}

	pGesture->device_setting_timer = TimerSet(pGesture->device_setting_timer, 0, 0, NULL, NULL);

	if( !pGesture->num_touches )
	{
		ErrorF("[X11][GestureTimerHandler] Failed to mt device information !\n");
		pGesture->device_setting_timer = TimerSet(pGesture->device_setting_timer, 0, 0, NULL, NULL);
		pGesture->first_fingerid = -1;
		return 0;
	}

	res = GestureSetMaxNumberOfFingers((int)pGesture->num_touches);

	if( !res )
	{
		ErrorF("[X11][GestureTimerHandler] Failed on GestureSetMaxNumberOfFingers(%d) !\n", (int)pGesture->num_touches);
		goto failed;
	}

	pGesture->finger_rects = (pixman_region16_t *)calloc(sizeof(pixman_region16_t), pGesture->num_touches);
	pGesture->fingers = (TouchStatus *)calloc(sizeof(TouchStatus), pGesture->num_touches);
	pGesture->event_sum = (int *)calloc(sizeof(int), pGesture->num_touches);

	if( !pGesture->finger_rects || !pGesture->fingers || !pGesture->event_sum)
	{
		ErrorF("[X11][GestureTimerHandler] Failed to allocate memory for finger information\n");
		goto failed;
	}

	pGesture->pRootWin = RootWindow(pGesture->master_pointer);
	g_pGesture->pTempWin = NULL;
	g_pGesture->inc_num_pressed = 0;

	if( ERROR_NONE != GestureRegionsInit() || ERROR_NONE != GestureInitEQ() )
	{
		goto failed;
	}

	mieqSetHandler(ET_TouchBegin, GestureHandleTouchBeginEvent);
	mieqSetHandler(ET_TouchEnd, GestureHandleTouchEndEvent);
	mieqSetHandler(ET_TouchUpdate, GestureHandleTouchUpdateEvent);

	if( pGesture->is_active )
		mieqSetHandler(ET_MTSync, GestureHandleMTSyncEvent);

	return 0;

failed:

	pGesture->num_touches = 0;
	GestureUninstallResourceStateHooks();
	GestureUnsetMaxNumberOfFingers();

	return 0;
}

BOOL
IsXTestDevice(DeviceIntPtr dev, DeviceIntPtr master)
{
	if (IsMaster(dev))
		return FALSE;

	if (master)
		return (dev->xtest_master_id == master->id);

	return (dev->xtest_master_id != 0);
}

void
GestureEnable(int enable, Bool prop, DeviceIntPtr dev)
{
	if((!enable) && (g_pGesture->is_active))
	{
		g_pGesture->ehtype = PROPAGATE_EVENTS;
		mieqSetHandler(ET_MTSync, NULL);
		g_pGesture->is_active = 0;
		ErrorF("[X11][GestureEnable] Disabled !\n");
	}
	else if((enable) && (!g_pGesture->is_active))
	{
		g_pGesture->ehtype = KEEP_EVENTS;
		mieqSetHandler(ET_MTSync, GestureHandleMTSyncEvent);
		g_pGesture->is_active = 1;
		ErrorF("[X11][GestureEnable] Enabled !\n");
	}

	if(!prop)
		 XIChangeDeviceProperty(dev, prop_gesture_recognizer_onoff, XA_INTEGER, 32, PropModeReplace, 1, &g_pGesture->is_active, FALSE);
}

ErrorStatus
GestureRegionsInit(void)
{
	int i;

	if( !g_pGesture )
		return ERROR_INVALPTR;

	pixman_region_init(&g_pGesture->area);

	for( i = 0 ; i < g_pGesture->num_touches; i++ )
	{
		pixman_region_init_rect (&g_pGesture->finger_rects[i], 0, 0, FINGER_WIDTH_2T, FINGER_HEIGHT_2T);
	}

	return ERROR_NONE;
}

ErrorStatus
GestureRegionsReinit(void)
{
	if( !g_pGesture )
	{
		ErrorF("[X11][GestureRegionsReinit] Invalid pointer access !\n");
		return ERROR_INVALPTR;
	}

	pixman_region_init(&g_pGesture->area);

	return ERROR_NONE;
}

ErrorStatus
GestureInitEQ(void)
{
	int i;
	IEventPtr tmpEQ;

	tmpEQ = (IEventRec *)calloc(GESTURE_EQ_SIZE, sizeof(IEventRec));

	if( !tmpEQ )
	{
		ErrorF("[X11][GestureInitEQ] Failed to allocate memory for EQ !\n");
		return ERROR_ALLOCFAIL;
	}

	for( i = 0 ; i < GESTURE_EQ_SIZE ; i++ )
	{
		tmpEQ[i].event = (InternalEvent *)malloc(sizeof(InternalEvent));
		if( !tmpEQ[i].event )
		{
			ErrorF("[X11][GestureInitEQ] Failed to allocation memory for each event buffer in EQ !\n");
			i--;
			while(i >= 0 && tmpEQ[i].event)
			{
				free(tmpEQ[i].event);
				tmpEQ[i].event = NULL;
			}
			free (tmpEQ);
			tmpEQ = NULL;
			return ERROR_ALLOCFAIL;
		}
	}

	g_pGesture->EQ = tmpEQ;
	g_pGesture->headEQ = g_pGesture->tailEQ = 0;

	return ERROR_NONE;
}

ErrorStatus
GestureFiniEQ(void)
{
	int i;

	if( !g_pGesture || !g_pGesture->EQ )
		return ERROR_INVALPTR;

	for( i = 0 ; i < GESTURE_EQ_SIZE ; i++ )
	{
		if( g_pGesture->EQ[i].event )
		{
			free(g_pGesture->EQ[i].event);
			g_pGesture->EQ[i].event = NULL;
		}
	}

	free(g_pGesture->EQ);
	g_pGesture->EQ = NULL;

	return ERROR_NONE;
}

ErrorStatus
GestureEnqueueEvent(int screen_num, InternalEvent *ev, DeviceIntPtr device)
{
	int tail;

	if( !g_pGesture || !g_pGesture->EQ )
	{
		ErrorF("[X11][GestureEnqueueEvent] Invalid pointer access !\n");
		return ERROR_INVALPTR;
	}

	tail = g_pGesture->tailEQ;

	if( tail >= GESTURE_EQ_SIZE )
	{
		ErrorF("[X11][GestureEnqueueEvent] Gesture EQ is full...Force Gesture Flush \n");
		printk("[X11][GestureEnqueueEvent] Gesture EQ is full...Force Gesture Flush !\n");
		GestureEventsFlush();
		return ERROR_EQFULL;
	}

#ifdef __DETAIL_DEBUG__
	switch( ev->any.type )
	{
		case ET_TouchBegin:
			ErrorF("[X11][GestureEnqueueEvent] ET_TouchBegin (id:%d)\n", device->id);
			break;

		case ET_TouchEnd:
			ErrorF("[X11][GestureEnqueueEvent] ET_TouchEnd (id:%d)\n", device->id);
			break;

		case ET_TouchUpdate:
			ErrorF("[X11][GestureEnqueueEvent] ET_TouchUpdate (id:%d)\n", device->id);
			break;
	}
#endif//__DETAIL_DEBUG__

	g_pGesture->EQ[tail].device = device;
	g_pGesture->EQ[tail].screen_num = screen_num;
	memcpy(g_pGesture->EQ[tail].event, ev, sizeof(InternalEvent));//need to be optimized
	g_pGesture->tailEQ++;

	return ERROR_NONE;
}

ErrorStatus
GestureEventsFlush(void)
{
	int i;
	DeviceIntPtr device;

	if( !g_pGesture->EQ )
	{
		ErrorF("[X11][GestureEventsFlush] Invalid pointer access !\n");
		return ERROR_INVALPTR;
	}

#ifdef __DETAIL_DEBUG__
	ErrorF("[X11][GestureEventsFlush]\n");
#endif//__DETAIL_DEBUG__

	for( i = g_pGesture->headEQ ; i < g_pGesture->tailEQ ; i++)
	{
		device = g_pGesture->EQ[i].device;
		device->public.processInputProc(g_pGesture->EQ[i].event, device);
	}

	for( i = 0 ; i < g_pGesture->num_touches; i++ )
		g_pGesture->event_sum[i] = 0;

	g_pGesture->headEQ = g_pGesture->tailEQ = 0;//Free EQ

	return ERROR_NONE;
}

void
GestureEventsDrop(void)
{
	g_pGesture->headEQ = g_pGesture->tailEQ = 0;//Free EQ
}

#ifdef HAVE_PROPERTIES
static void
GestureInitProperty(DeviceIntPtr dev)
{
	int rc;

	prop_gesture_recognizer_onoff = MakeAtom(GESTURE_RECOGNIZER_ONOFF, strlen(GESTURE_RECOGNIZER_ONOFF),  TRUE);
	rc = XIChangeDeviceProperty(dev, prop_gesture_recognizer_onoff, XA_INTEGER, 32, PropModeReplace, 1, &g_pGesture->is_active, FALSE);

	if (rc != Success)
		return;

	XISetDevicePropertyDeletable(dev, prop_gesture_recognizer_onoff, FALSE);
}

static int
GestureSetProperty(DeviceIntPtr dev, Atom atom, XIPropertyValuePtr val,
                 BOOL checkonly)
{
	InputInfoPtr pInfo  = dev->public.devicePrivate;

	if( prop_gesture_recognizer_onoff == atom )
	{
		int data;
		if( val->format != 32 || val->type != XA_INTEGER || val->size != 1 )
			return BadMatch;

		if( !checkonly )
		{
			data = *((int *)val->data);
			GestureEnable(data, TRUE, dev);
		}
	}

	return Success;
}
#endif//HAVE_PROPERTIES

static int
GestureInit(DeviceIntPtr device)
{
	InputInfoPtr pInfo;
	pInfo = device->public.devicePrivate;

#ifdef HAVE_PROPERTIES
	GestureInitProperty(device);
	XIRegisterPropertyHandler(device, GestureSetProperty, NULL, NULL);
#endif

	return Success;
}

static void
GestureFini(DeviceIntPtr device)
{
	XIRegisterPropertyHandler(device, NULL, NULL, NULL);
}

static pointer
GesturePlug(pointer module, pointer options, int *errmaj, int *errmin)
{
	xf86AddInputDriver(&GESTURE, module, 0);
	return module;
}

static void
GestureUnplug(pointer p)
{
}

static int
GesturePreInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
    int rc = BadAlloc;
    GestureDevicePtr    pGesture;

    pGesture = calloc(1, sizeof(GestureDeviceRec));

    if (!pGesture) {
        pInfo->private = NULL;
        //xf86DeleteInput(pInfo, 0);
        goto error;
    }

    g_pGesture = pGesture;
    pInfo->private = pGesture;
    pInfo->flags = 0;
    pInfo->read_input = GestureReadInput; /* new data avl */
    pInfo->switch_mode = NULL; /* toggle absolute/relative mode */
    pInfo->device_control = GestureControl; /* enable/disable dev */
    /* process driver specific options */
    pGesture->device = xf86SetStrOption(pInfo->options, "Device", "/dev/null");
    pGesture->is_active = xf86SetIntOption(pInfo->options, "Activate", 0);
    pGesture->gestureWin = None;
    pGesture->lastSelectedWin = None;
    pGesture->num_touches = GESTURE_MAX_TOUCH;
    g_pGesture->grabMask = g_pGesture->eventMask = 0;

    xf86Msg(X_INFO, "%s: Using device %s.\n", pInfo->name, pGesture->device);

    /* process generic options */
    xf86CollectInputOptions(pInfo, NULL);
    xf86ProcessCommonOptions(pInfo, pInfo->options);

    pInfo->fd = -1;
    return Success;

error:
    if (pInfo->fd >= 0)
        close(pInfo->fd);
    return rc;
}

static void
GestureUnInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
	GestureDevicePtr pGesture = pInfo->private;

	g_pGesture = pGesture = NULL;
	pInfo->private = NULL;

	xf86DeleteInput(pInfo, 0);
}

static int
GestureControl(DeviceIntPtr device, int what)
{
    InputInfoPtr  pInfo = device->public.devicePrivate;
    GestureDevicePtr pGesture = pInfo->private;

    switch(what)
    {
        case DEVICE_INIT:
	     GestureInit(device);
            break;

        /* Switch device on.  Establish socket, start event delivery.  */
        case DEVICE_ON:
            xf86Msg(X_INFO, "%s: On.\n", pInfo->name);

            if (device->public.on)
                    break;

            device->public.on = TRUE;
	     pGesture->this_device = device;
	     GestureSetMaxNumberOfFingers((int)pGesture->num_touches);
	     if( ERROR_ABNORMAL == GestureEnableEventHandler(pInfo) )
	     	goto device_off;
            break;

       case DEVICE_OFF:
device_off:
	     GestureDisableEventHandler();
	     GestureFini(device);
	     pGesture->this_device = NULL;
             xf86Msg(X_INFO, "%s: Off.\n", pInfo->name);

            if (!device->public.on)
                break;

            pInfo->fd = -1;
            device->public.on = FALSE;
            break;

      case DEVICE_CLOSE:
            /* free what we have to free */
            break;
    }
    return Success;
}

static void
GestureReadInput(InputInfoPtr pInfo)
{
}

