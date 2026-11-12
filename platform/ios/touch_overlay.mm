// The iOS on-screen gamepad: platform/touch_layout.h (the Android app's TouchLayout in C++) drawn with Core Graphics
// in a transparent view over the game's window, in the launcher's colours (as the Android TouchControls view). It is
// the touch pad the game reads as joystick GLFW_JOYSTICK_LAST (glfw_sdl_ios.cpp). A game controller or a hardware
// keyboard hides it, a touch on the screen brings it back. (Not yet: Android's layout editor.)
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include "ios_app.h"
#include "../touch_layout.h"

#include <algorithm>
#include <chrono>
#include <mutex>

using cane::touch::Layout;

namespace {
std::mutex touchMutex;
cane::ios::TouchState published;
std::chrono::steady_clock::time_point lastTouch{};
bool wantVisible = true;   // (the shim: false after physical input, true after a touch)
bool physicalPad = false;  // (a controller or keyboard is connected)
// Launcher tokens (launcher/ui/theme.h), as TouchControls.java.
constexpr unsigned SURFACE = 0x0D172C, RAISED = 0x13213D, BORDER = 0x2A4675, RIM = 0x4A78B8, CTA = 0x02BCFC,
                   ICE = 0x54D2FD, TEXT = 0xF2F7FF, TEXT2 = 0xA9BCD9, MUTED = 0x6F84A8;
constexpr unsigned TRIANGLE = 0x3DDBB1, CIRCLE = 0xFF6B81, CROSS = 0x86AEFF, SQUARE = 0xF39AD6;
UIColor* color(unsigned rgb, CGFloat alpha) {
    return [UIColor colorWithRed:((rgb >> 16) & 255) / 255.0 green:((rgb >> 8) & 255) / 255.0 blue:(rgb & 255) / 255.0
                           alpha:std::clamp<CGFloat>(alpha, 0, 1)];
}
}  // namespace

@interface SHTouchOverlay : UIView
@property(nonatomic) CADisplayLink* holdLink;
@end

@implementation SHTouchOverlay {
    Layout _layout;
    std::array<float, Layout::AXES> _axes;
    std::array<bool, Layout::BUTTONS> _buttons;
    std::array<bool, Layout::BUTTONS> _pressedBefore;
    CFTimeInterval _holdStart;
    UIImpactFeedbackGenerator* _tap;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.opaque = NO;
        self.backgroundColor = UIColor.clearColor;
        self.multipleTouchEnabled = YES;
        self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        self.contentMode = UIViewContentModeRedraw;
        _axes = {0, 0, 0, 0, -1, -1};
        _buttons.fill(false);
        _pressedBefore.fill(false);
        _holdStart = 0;
        _tap = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
        NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
        if ([d objectForKey:@"touch.scale"]) _layout.setScale([d floatForKey:@"touch.scale"]);
        if ([d objectForKey:@"touch.opacity"]) _layout.setOpacity([d floatForKey:@"touch.opacity"]);
        if ([d objectForKey:@"touch.haptics"]) _layout.haptics = [d boolForKey:@"touch.haptics"];
    }
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    const UIEdgeInsets safe = self.safeAreaInsets;
    _layout.layout(self.bounds.size.width, self.bounds.size.height, safe.left, safe.top, safe.right, safe.bottom);
    [self setNeedsDisplay];
}
- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    [self setNeedsLayout];
}
- (void)setContext:(int)context {
    if (context == _layout.context()) return;
    _layout.setContext(context);
    [self publish];
    [self setNeedsDisplay];
}
- (void)releaseAll {
    _layout.cancelAll();
    _layout.compute(_axes, _buttons);
    _pressedBefore.fill(false);
    [self publish];
}
- (void)publish {
    _layout.compute(_axes, _buttons);
    bool newlyPressed = false;
    {
        std::lock_guard lock(touchMutex);
        published.active = !self.hidden;
        for (int i = 0; i < Layout::AXES; ++i) published.axes[i] = _axes[i];
        for (int b = 0; b < Layout::BUTTONS; ++b) {
            published.buttons[b] = _buttons[b];
            newlyPressed |= _buttons[b] && !_pressedBefore[b];
            _pressedBefore[b] = _buttons[b];
        }
    }
    if (newlyPressed && _layout.haptics) [_tap impactOccurred];
    [self setNeedsDisplay];
}
- (void)handleTouches:(NSSet<UITouch*>*)touches phase:(int)phase {
    {
        std::lock_guard lock(touchMutex);
        lastTouch = std::chrono::steady_clock::now();
    }
    for (UITouch* touch in touches) {
        const CGPoint p = [touch locationInView:self];
        const long id = long(reinterpret_cast<intptr_t>((__bridge void*)touch));
        if (phase == 0) _layout.down(id, p.x, p.y);
        else if (phase == 1) _layout.move(id, p.x, p.y);
        else _layout.up(id);
    }
    [self publish];
    const bool holding = _layout.held(Layout::HOLD);
    if (holding && !self.holdLink) {
        self.holdLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(setNeedsDisplay)];
        [self.holdLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    } else if (!holding && self.holdLink) {
        [self.holdLink invalidate];
        self.holdLink = nil;
    }
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { [self handleTouches:touches phase:0]; }
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { [self handleTouches:touches phase:1]; }
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { [self handleTouches:touches phase:2]; }
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { [self handleTouches:touches phase:2]; }

// ---- Drawing (TouchControls.java onDraw)
- (void)disc:(CGContextRef)c x:(CGFloat)x y:(CGFloat)y r:(CGFloat)r pressed:(bool)pressed alpha:(CGFloat)alpha {
    CGRect box = CGRectMake(x - r, y - r, 2 * r, 2 * r);
    [(pressed ? color(CTA, .42 * alpha) : color(SURFACE, .62 * alpha)) setFill];
    CGContextFillEllipseInRect(c, box);
    CGContextSetLineWidth(c, _layout.unit * .006);
    [(pressed ? color(ICE, alpha) : color(RIM, .85 * alpha)) setStroke];
    CGContextStrokeEllipseInRect(c, box);
}
- (void)label:(NSString*)text x:(CGFloat)x y:(CGFloat)y size:(CGFloat)size color:(UIColor*)textColor {
    NSDictionary* attributes = @{NSFontAttributeName : [UIFont boldSystemFontOfSize:size], NSForegroundColorAttributeName : textColor};
    const CGSize s = [text sizeWithAttributes:attributes];
    [text drawAtPoint:CGPointMake(x - s.width / 2, y - s.height / 2) withAttributes:attributes];
}
- (void)pill:(CGContextRef)c element:(int)e label:(NSString*)text pressed:(bool)pressed alpha:(CGFloat)alpha {
    const CGFloat hw = _layout.halfWidth[e], hh = _layout.halfHeight[e];
    UIBezierPath* path = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(_layout.x[e] - hw, _layout.y[e] - hh, 2 * hw, 2 * hh)
                                                    cornerRadius:hh];
    [(pressed ? color(CTA, .42 * alpha) : color(SURFACE, .62 * alpha)) setFill];
    [path fill];
    path.lineWidth = _layout.unit * .006;
    [(pressed ? color(ICE, alpha) : color(RIM, .85 * alpha)) setStroke];
    [path stroke];
    [self label:text x:_layout.x[e] y:_layout.y[e] size:hh * (text.length > 2 ? .78 : 1.02) color:color(TEXT, alpha)];
}
- (void)symbol:(CGContextRef)c index:(int)index x:(CGFloat)x y:(CGFloat)y r:(CGFloat)r alpha:(CGFloat)alpha {
    const CGFloat s = r * .42;
    CGContextSetLineWidth(c, r * .15);
    CGContextSetLineCap(c, kCGLineCapRound);
    switch (index) {
    case 0:  // cross
        [color(CROSS, alpha) setStroke];
        CGContextMoveToPoint(c, x - s, y - s), CGContextAddLineToPoint(c, x + s, y + s);
        CGContextMoveToPoint(c, x - s, y + s), CGContextAddLineToPoint(c, x + s, y - s);
        CGContextStrokePath(c);
        break;
    case 1:  // circle
        [color(CIRCLE, alpha) setStroke];
        CGContextStrokeEllipseInRect(c, CGRectMake(x - s * 1.08, y - s * 1.08, s * 2.16, s * 2.16));
        break;
    case 2:  // square
        [color(SQUARE, alpha) setStroke];
        CGContextStrokeRect(c, CGRectMake(x - s * .92, y - s * .92, s * 1.84, s * 1.84));
        break;
    default:  // triangle
        [color(TRIANGLE, alpha) setStroke];
        CGContextMoveToPoint(c, x, y - s * 1.08);
        CGContextAddLineToPoint(c, x + s * 1.1, y + s * .78);
        CGContextAddLineToPoint(c, x - s * 1.1, y + s * .78);
        CGContextClosePath(c);
        CGContextStrokePath(c);
    }
}
- (void)stick:(CGContextRef)c element:(int)e alpha:(CGFloat)alpha {
    const bool held = _layout.held(e);
    const CGFloat radius = e == Layout::STICK ? _layout.stickRadius : _layout.cameraRadius;
    const CGFloat cx = held ? _layout.anchorX(e) : _layout.x[e], cy = held ? _layout.anchorY(e) : _layout.y[e];
    if (e == Layout::CAMERA && !held) {
        // idle camera hint: two orbit arcs, quieter than the real controls
        const CGFloat a = alpha * .55, r = radius * .62;
        CGContextSetLineWidth(c, _layout.unit * .007);
        [color(TEXT2, a) setStroke];
        CGContextAddArc(c, cx, cy, r, 200 * M_PI / 180, 310 * M_PI / 180, 0);
        CGContextStrokePath(c);
        CGContextAddArc(c, cx, cy, r, 20 * M_PI / 180, 130 * M_PI / 180, 0);
        CGContextStrokePath(c);
        [self label:@"CAMERA" x:cx y:cy + radius * 1.02 size:_layout.unit * .028 * _layout.scale color:color(MUTED, a)];
        return;
    }
    const CGRect base = CGRectMake(cx - radius, cy - radius, 2 * radius, 2 * radius);
    [color(SURFACE, .45 * alpha) setFill];
    CGContextFillEllipseInRect(c, base);
    CGContextSetLineWidth(c, _layout.unit * .006);
    [color(held ? CTA : RIM, .8 * alpha) setStroke];
    CGContextStrokeEllipseInRect(c, base);
    const int axis = e == Layout::STICK ? 0 : 2;
    const CGFloat kx = cx + _layout.axis(axis) * radius, ky = cy + _layout.axis(axis + 1) * radius;
    const CGFloat knob = e == Layout::STICK ? _layout.knobRadius : _layout.knobRadius * .8;
    const CGRect k = CGRectMake(kx - knob, ky - knob, 2 * knob, 2 * knob);
    [color(held ? CTA : RAISED, (held ? .55 : .85) * alpha) setFill];
    CGContextFillEllipseInRect(c, k);
    [color(held ? ICE : RIM, alpha) setStroke];
    CGContextStrokeEllipseInRect(c, k);
}
- (void)dpad:(CGContextRef)c alpha:(CGFloat)alpha {
    const CGFloat half = _layout.dpadHalf;
    for (int d = 0; d < 4; ++d) {
        const CGFloat cx = _layout.dpadX(d), cy = _layout.dpadY(d);
        const bool pressed = _buttons[Layout::DPAD_BUTTONS[d]];
        UIBezierPath* path = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(cx - half, cy - half, 2 * half, 2 * half) cornerRadius:half * .35];
        [(pressed ? color(CTA, .42 * alpha) : color(SURFACE, .62 * alpha)) setFill];
        [path fill];
        path.lineWidth = _layout.unit * .006;
        [color(pressed ? ICE : RIM, .85 * alpha) setStroke];
        [path stroke];
        const CGFloat dx = Layout::DPAD_DIRECTIONS[d][0], dy = Layout::DPAD_DIRECTIONS[d][1], s = half * .42;
        [color(TEXT, alpha) setFill];
        CGContextMoveToPoint(c, cx + dx * s, cy + dy * s);
        CGContextAddLineToPoint(c, cx - dx * s * .6 - dy * s, cy - dy * s * .6 + dx * s);
        CGContextAddLineToPoint(c, cx - dx * s * .6 + dy * s, cy - dy * s * .6 - dx * s);
        CGContextClosePath(c);
        CGContextFillPath(c);
    }
}
- (void)hold:(CGContextRef)c {
    // a scene: a ring that closes over the 2 s hold the game needs to skip it, above the finger
    if (!_layout.held(Layout::HOLD)) {
        _holdStart = 0;
        return;
    }
    const CFTimeInterval now = CACurrentMediaTime();
    if (_holdStart == 0) _holdStart = now;
    const CGFloat u = _layout.unit, r = u * .055;
    const CGFloat cx = std::clamp<CGFloat>(_layout.pointerX(Layout::HOLD), r * 1.6, self.bounds.size.width - r * 1.6);
    const CGFloat cy = std::clamp<CGFloat>(_layout.pointerY(Layout::HOLD) - u * .2, r * 1.6, self.bounds.size.height - r * 1.6);
    const CGFloat progress = std::min<CGFloat>(1, (now - _holdStart) / 2.0);
    const CGRect ring = CGRectMake(cx - r * 1.25, cy - r * 1.25, r * 2.5, r * 2.5);
    [color(SURFACE, .7) setFill];
    CGContextFillEllipseInRect(c, ring);
    CGContextSetLineWidth(c, u * .008);
    [color(BORDER, .9) setStroke];
    CGContextStrokeEllipseInRect(c, ring);
    [color(CTA, 1) setStroke];
    CGContextAddArc(c, cx, cy, r * 1.25, -M_PI / 2, -M_PI / 2 + 2 * M_PI * progress, 0);
    CGContextStrokePath(c);
    [color(TEXT, 1) setFill];
    [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(cx - r * .2, cy - r * .3, r * .4, r * 1.25) cornerRadius:r * .2] fill];
}
- (void)drawRect:(CGRect)rect {
    if (_layout.unit <= 1) return;
    CGContextRef c = UIGraphicsGetCurrentContext();
    const CGFloat alpha = _layout.opacity;
    if (_layout.visible(Layout::CAMERA)) [self stick:c element:Layout::CAMERA alpha:alpha];
    if (_layout.visible(Layout::STICK)) [self stick:c element:Layout::STICK alpha:alpha];
    if (_layout.visible(Layout::DPAD)) [self dpad:c alpha:alpha];
    if (_layout.visible(Layout::FACE))
        for (int b = 0; b < 4; ++b) {
            const bool pressed = _buttons[Layout::FACE_BUTTONS[b]];
            [self disc:c x:_layout.faceX(b) y:_layout.faceY(b) r:_layout.faceRadius pressed:pressed alpha:alpha];
            [self symbol:c index:b x:_layout.faceX(b) y:_layout.faceY(b) r:_layout.faceRadius alpha:pressed ? 1 : alpha * 1.15];
        }
    if (_layout.visible(Layout::L2)) [self pill:c element:Layout::L2 label:@"L2" pressed:_axes[4] > 0 alpha:alpha];
    if (_layout.visible(Layout::L1)) [self pill:c element:Layout::L1 label:@"L1" pressed:_buttons[Layout::LB] alpha:alpha];
    if (_layout.visible(Layout::R2)) [self pill:c element:Layout::R2 label:@"R2" pressed:_axes[5] > 0 alpha:alpha];
    if (_layout.visible(Layout::R1)) [self pill:c element:Layout::R1 label:@"R1" pressed:_buttons[Layout::RB] alpha:alpha];
    if (_layout.visible(Layout::SELECT)) [self pill:c element:Layout::SELECT label:@"SELECT" pressed:_buttons[Layout::BACK] alpha:alpha];
    if (_layout.visible(Layout::START_KEY)) [self pill:c element:Layout::START_KEY label:@"START" pressed:_buttons[Layout::START] alpha:alpha];
    if (_layout.visible(Layout::L3)) {
        [self disc:c x:_layout.x[Layout::L3] y:_layout.y[Layout::L3] r:_layout.clickRadius pressed:_buttons[Layout::L3_BUTTON] alpha:alpha];
        [self label:@"L3" x:_layout.x[Layout::L3] y:_layout.y[Layout::L3] size:_layout.clickRadius * .72 color:color(TEXT, alpha)];
    }
    if (_layout.visible(Layout::R3)) {
        [self disc:c x:_layout.x[Layout::R3] y:_layout.y[Layout::R3] r:_layout.clickRadius pressed:_buttons[Layout::R3_BUTTON] alpha:alpha];
        [self label:@"R3" x:_layout.x[Layout::R3] y:_layout.y[Layout::R3] size:_layout.clickRadius * .72 color:color(TEXT, alpha)];
    }
    [self hold:c];
}
@end

namespace {
SHTouchOverlay* overlay = nil;
id connectObserver = nil, disconnectObserver = nil;
void applyVisibility() {
    if (!overlay) return;
    const bool show = wantVisible && !physicalPad;
    if (overlay.hidden == !show) return;
    overlay.hidden = !show;
    [overlay releaseAll];
}
void detectPhysical() {
    bool found = GCController.controllers.count > 0;
    if (@available(iOS 14.0, *)) found = found || GCKeyboard.coalescedKeyboard != nil;
    physicalPad = found;
    // (a pad plugged in replaces the touch pad; with none left the touch pad comes back)
    if (!found) wantVisible = true;
    applyVisibility();
}
void onMain(void (^block)(void)) {
    if (NSThread.isMainThread) block();
    else dispatch_async(dispatch_get_main_queue(), block);
}
}  // namespace

namespace cane::ios {
TouchState touchState() {
    std::lock_guard lock(touchMutex);
    return published;
}
bool touchInputRecently() {
    std::lock_guard lock(touchMutex);
    return std::chrono::steady_clock::now() - lastTouch < std::chrono::milliseconds(250);
}
void setTouchControlsVisible(bool visible) {
    onMain(^{
      wantVisible = visible;
      applyVisibility();
    });
}
void setTouchContext(TouchContext context) {
    static int last = -1;
    if (int(context) == last) return;
    last = int(context);
    onMain(^{
      [overlay setContext:int(context)];
    });
}
void attachTouchOverlay(void* uiWindow) {
    onMain(^{
      UIWindow* window = (__bridge UIWindow*)uiWindow;
      if (!window) {
          [overlay.holdLink invalidate];
          [overlay removeFromSuperview];
          overlay = nil;
          std::lock_guard lock(touchMutex);
          published = {};
          return;
      }
      if (!overlay) overlay = [[SHTouchOverlay alloc] initWithFrame:window.bounds];
      overlay.frame = window.bounds;
      [window addSubview:overlay];
      [window bringSubviewToFront:overlay];
      if (!connectObserver) {
          NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
          connectObserver = [center addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                                            usingBlock:^(NSNotification* note) {
                                                (void)note;
                                                detectPhysical(); }];
          disconnectObserver = [center addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                                               usingBlock:^(NSNotification* note) {
                                                (void)note;
                                                detectPhysical(); }];
          if (@available(iOS 14.0, *)) {
              [center addObserverForName:GCKeyboardDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                              usingBlock:^(NSNotification* note) {
                                                (void)note;
                                                detectPhysical(); }];
              [center addObserverForName:GCKeyboardDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                              usingBlock:^(NSNotification* note) {
                                                (void)note;
                                                detectPhysical(); }];
          }
      }
      overlay.hidden = NO;
      detectPhysical();
      [overlay publish];
      cane::ios::log(std::string("touch controls: attached, ") + (overlay.hidden ? "hidden (a controller or keyboard is connected)" : "shown"));
    });
}
}  // namespace cane::ios
