# pwm-gpio
Linux kernel module for soft pwm on gpio usign high resolution timers.

## Usage example

Move a servo motor at 50hz pwm

    echo 0 > /sys/class/pwm/pwmchip0/export
    echo 20000000 > /sys/class/pwm/pwmchip0/pwm0/period
    echo 1500000 > /sys/class/pwm/pwmchip0/pwm0/duty_cycle
    echo 1 > /sys/class/pwm/pwmchip0/pwm0/enable
