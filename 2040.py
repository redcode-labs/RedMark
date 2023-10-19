import board as bd
import digitalio as dio
from analogio import AnalogIn
from time import sleep
import supervisor as sv
import busio as bus
from adafruit_debouncer import Debouncer
import microcontroller as uC
from adafruit_pn532.uart import PN532_UART
import adafruit_bitbangio as bang

MAXTEMP = 65.5
MAXVOL = 5.15
__loop = True
jmpr_first_removal = False
slider = bd.A3
mov = bd.A0
mov_pwr = dio.DigitalInOut(bd.GP11)
rx = bd.GP0
tx = bd.GP1
sda = bd.GP14
scl = bd.GP28
pn532_baud = 115200 #9600
uart_timeout = 2
uart = bus.UART(tx, rx, baudrate=baud)
green = dio.DigitalInOut(bd.GP20)
red = dio.DigitalInOut(bd.GP19)
yellow = dio.DigitalInOut(bd.GP17)
yellow_gnd = dio.DigitalInOut(bd.GP15) 
grounds = [yellow_gnd]
jmpr = dio.DigitalInOut(bd.GP8)
jmpr.direction = dio.Direction.INPUT
#en = dio.DigitalInOut(bd.GP26)
#en.pull = dio.Pull.DOWN
jmpr_pwr = dio.DigitalInOut(bd.GP6)
outputs = [mov_pwr, jmper_pwr]
leds = [red, green, yellow]
inputs = [slider, mov]
analogs = {}
pn532 = None
samconf = True
tag_listen_timeout = 1

def set_pin_directions():
    for out in outputs:
        out.direction = dio.Direction.OUTPUT
        out.value = True
    for led in leds:
        led.direction = dio.Direction.OUTPUT
    for inp in inputs:
        analogs[inp] = AnalogIn(inp)
    for g in grounds:
        ground(g)
    
def ground(pin):
    pin.direction = dio.Direction.OUTPUT
    pin.value = False
    
def get_voltage(pin):
    return (pin.value * 3.3) / 65536

def ledson():
    yellow.value = True
    red.value = True
    green.value = True
    
def ledsoff():
    yellow.value = False
    red.value = False
    green.value = False
    
def blink_all():
    ledson()
    sleep(1.50)
    ledsoff()
    
def blink(led, n):
    initial_state = led.value
    i = 0
    while i <= n:
        led.value = True
        sleep(0.5)
        i += 1
        led.value = False
    led.value = initial_state
        
def blink_fast(led, n):
    initial_state = led.value
    i = 0
    while i <= n:
        led.value = True
        sleep(0.23)
        i+=1
        led.value = False
    led.value = initial_state
    
def blink_slow(led, n):
    initial_state = led.value
    i = 0
    while i <= n:
        led.value = True
        sleep(0.95)
        i+=1
        led.value = False
    led.value = initial_state
    
def blink_one(led, n):
    for l in leds:
        if l != led:
            l.value = False
    blink_fast(led, n)
    
def rainbow(repeats):
    for r in range(0,repeats):
        ledsoff()
        for led in leds:
            led.value = True
            sleep(0.25)
            led.value = False
        sleep(0.7)

def get_mov():
    if analogs[mov].value > 36000:
        return True
    #if analogs[mov].value != init_mov_val:
    if abs(analogs[mov].value - init_mov_val) > 25000:
        return True
    return False

def check_uart():
    if uart.read(32) is not None:
        blink_one(green,5)
        green.value = True
        return green.value
    sleep(1)
    blink_one(red, 5)
    return False

def check_usb():
    blink(green,2)
    if sv.runtime.usb_connected:
        blink_long(yellow, 1)
        return True
    else:
        blink_long(green, 1)
    return False

def reboot():
    blink_fast(red, 5)
    uC.on_next_reset(uC.RunMode.NORMAL)
    uC.reset()
    
def enter_uf2():
    if not sv.runtime.usb_connected:
        uC.on_next_reset(uC.RunMode.UF2)
        uC.reset()

def check_reset_reason():
    pass

def diagnostics():
    phases = [check_usb,
              check_reset_reason,
              check_temp_and_volt]
    ledson()
    sleep(1.3)
    ledsoff()
    phaseno = 1
    for p in phases:
        p()
        rainbow(phaseno)
        phaseno += 1
    ledsoff()

#change_percent = ((float(current)-previous)/previous)*100
        
def debounce(pin):
    pin.pull = dio.Pull.UP
    button = Debouncer(pin)
    while True:
        button.update()
        if button.fell:
            return True
        if button.rose:
            return False
        
def check_movement():
    red.value = False
    while not red.value:
        sleep(15)
        jmpr_first_removal = True
        if get_mov():
            red.value = True
            break

def check_temp_and_volt():
    # . . .
    second_cpu_check = False
    for cpunit in uC.cpus:
        if second_cpu_check:
            yellow.value = True
        if cpunit.temperature > MAXTEMP:
            ledsoff()
            red.value = True
            blink_fast(red, 3)
            reboot()
        if cpunit.voltage > MAXVOLT:
            yellow.value = True
            blink_fast(red, 3)
            reboot()
        second_cpu_check = True

def no_jmpr_mode():
    if not jmpr_first_removal:
        print("[&] Launching diagnostics . . .")
        diagnostics()
    ledsoff()
    check_movement()       

def initialize():
    set_pin_directions()
    check_usb()
    check_temp_and_volt()
    if not jmpr.value:
        check_reset_reason()
    init_mov_val = analogs[mov].value
    yellow.value = False
    green.value = False

def pn532_init():
    try:
        uart = busio.UART(rx, tx, baudrate=pn532_baud, timeout=uart_timeout) 
        pn532 = PN532_UART(uart, debug=False)
        ic, ver, rev, support = pn532.firmware_version
        print("Found PN532 with firmware version: {0}.{1} -> 0x{2} [IC: {3}]".format(ver, rev, support, ic))
        if samconf:
            pn532.SAM_configuration()
        blink(green, 2)
        green.value = True
    except:
        blink(yellow, 2)
        yellow.value = True

def uid_detection_mode():
    while True:
        if not jmpr.value:
            break
        if pn532 == None:
            break
        listener = pn532.listen_for_passive_target(timeout=tag_listen_timeout)
        if pn532.uid_4b():
            print("[*] 4 bytes UID")
            blink_long(green, 2)
        if pn532.uid_7b():
            print("[*] 7 bytes UID")
            blink_long(yellow, 2)

def core():
    while __loop:
        if not jmpr.value:
            no_jmpr_mode()
        else:
            pn532_init()
            uid_detection_mode()
        
initialize()
core()
    
    