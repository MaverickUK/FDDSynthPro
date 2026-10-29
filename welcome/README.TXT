 _____  ____   ____      ____  __   __ _   _  _____  _   _
|  ___||  _ \ |  _ \    / ___| \ \ / /| \ | ||_   _|| | | |
| |_   | | | || | | |   \___ \  \ V / |  \| |  | |  | |_| |
|  _|  | |_| || |_| |    ___) |  | |  | |\  |  | |  |  _  |
|_|    |____/ |____/    |____/   |_|  |_| \_|  |_|  |_| |_|
                                       ____   ____    ___
                                      |  _ \ |  _ \  / _ \
                                      | |_) || |_) || | | |
                                      |  __/ |  _ < | |_| |
                                      |_|    |_| \_\ \___/

          A floppy drive, served from a microSD card, that sounds
                            like the real thing.
                              Firmware {VERSION}

==============================================================================

WHY YOU'RE SEEING THIS

This is the drive's welcome disk. The drive serves it when it starts without
a microSD card, or when the card has no disk image it can use. Anything you
save here is kept only until the drive restarts.

GETTING STARTED

1. Turn the drive off.
2. On a computer, copy disk images to the root folder of a microSD card
   (FAT32 or exFAT). An image is a raw copy of a whole floppy disk.
3. Put the card in the drive and turn it on. It serves the first image, in
   name order; choose another on the front panel.

DISK IMAGES

  .IMG .IMA   PC disks, known by their size:
              160K 180K 320K 360K (5.25-inch, 40 tracks)
              720K 800K 1.44M 1.68M (3.5-inch)   1.2M (5.25-inch, 360 RPM)
  .ADF        Amiga disks (880K)
  IMG.CFG     Other machines' disks (Roland, Akai, Ensoniq, Kaypro...): put
              GoTek's IMG.CFG, as FlashFloppy uses it, in the card's root.

THE FRONT PANEL

  Turn         the volume; in a list, move up and down
  Press        the list of disks (on the sounds screen: the sample packs)
  Action       the next screen; in a list, back
  Long action  eject the disk, or put it back

SETTINGS

The drive keeps its settings in FDDSYNTH.CFG on the card. It remembers the
disk, the sample pack and the volume by itself. Edit the file on a computer
to set the rest. There's a sample FDDSYNTH.CFG on this disk, with notes:
copy it to the card's root folder and edit it there.

  image=GAMES.IMG    the disk to start with, every time (else the last one)
  image2=WORK.IMG    the second drive's, with drive=a,b
  interface=ibmpc    ibmpc (a PC), shugart (most other machines: Atari ST,
                     Amstrad, synths and samplers) or amiga
  cable=twisted      ibmpc: twisted (the end of a PC cable) or straight
  drive=a            a, b or a,b (two drives); ds0-ds2 (shugart);
                     df0, df1 (amiga)

SOUNDS

Sample packs go in folders under /samples on the card, one folder a pack:
spinup.wav, spin.wav, spindown.wav, step.wav, seek.wav and bump.wav
(insert.wav and eject.wav if you like): 16-bit mono WAVs, one sample rate.

MORE

https://github.com/MaverickUK/FDDSynthPro
