# asus_tuf_kbd_backlight

Linux driver and daemon for the RGB keyboard backlight of the ASUS TUF Gaming 16 FX610JH.

The backlight is an ITE51368 I2C HID LampArray controller (`0B05:19B6`). The driver takes host control of it and exposes it as a multicolor LED: `/sys/class/leds/asus:rgb:kbd_color`.

On/off and the 3 brightness steps (Fn+F2/F3) stay with asus-wmi (`asus::kbd_backlight`). This driver handles color, fine intensity and effects.

## Build and install

```bash
sudo make dkms
sudo make install
sudo udevadm control --reload
sudo usermod -aG video $USER
sudo systemctl enable --now tuf_kbdd
```

Log out and back in for the group change to apply.

## Usage

```bash
tuf_kbdctl color ff6400
tuf_kbdctl color red
tuf_kbdctl brightness 200
tuf_kbdctl next
tuf_kbdctl status
```

The daemon restores the last color at boot and cycles presets on Fn+Aura.

## Effects

Load the LED trigger modules:

```bash
sudo modprobe -a ledtrig-timer ledtrig-pattern ledtrig-heartbeat
```

Breathing:

```bash
cd /sys/class/leds/asus:rgb:kbd_color
echo pattern > trigger
echo "0 1000 255 1000" | sudo tee pattern
```

## Module parameters

- `resume_interval_ms` (default 50)
- `resume_window_ms` (default 4000)

These control how often and for how long the color is re-applied after resume.

## Tested on

- ASUS TUF Gaming 16 FX610JH, BIOS 302
- Arch Linux, kernel 6.18 LTS
