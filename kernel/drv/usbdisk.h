/* USB disk mode: the SD card as a drive on a PC.
 *
 * Hold `d` while the device starts and it boots into this instead of
 * CardOS: the USB port stops being the serial console and becomes a mass
 * storage device, the card's own sectors behind it, so a PC copies music
 * and pictures at USB speed rather than over WiFi or the serial link. The
 * PC owns the card's filesystem for as long as this runs, so CardOS does
 * not mount it; a key press restarts into CardOS proper, with the serial
 * console back. Eject the drive on the PC first. */
#ifndef CARDOS_USBDISK_H
#define CARDOS_USBDISK_H

/* Never returns: it ends in a restart. */
void usbdisk_run(void);

#endif /* CARDOS_USBDISK_H */
