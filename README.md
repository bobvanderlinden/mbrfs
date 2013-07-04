# mbrfs

A FUSE-filesystem provides the partitions from a disk device or image.

## Motivation

When creating images that hold a filesystem, I always needed to use root to create a loopback device. From the loopback device I could access the different partitions of that image. I did this in an automated build process and didn't like that it needed root. FUSE allows users to mount filesystems and with mbrfs you can get the functionality of dividing devices into partitions, like loopback devices have.

## Compiling

    $ make

## Usage

    mbrfs DEVICE MOUNTPOINT [FUSE OPTIONS...]

* `DEVICE` is the block device or image that has a MBR partition table with multiple partitions
* `MOUNTPOINT` is a directory where the partitions will be located.

To unmount mbrfs use `fusermount -u MOUNTPOINT`.

### Example

Say you want to create an image with two FAT partitions. We can do the following:

First create a file of 100MB:

    $ fallocate --length 100M test.img

Now create a partition table on the image using `cfdisk`. This will allow you to interactively create the 2 partitions:

	$ cfdisk test.img

Next, we use mbrfs to be able to access those partitions:

	$ mkdir test
	$ mbrfs test.img test

`mbrfs` will have created the following files:

	test/1
	test/2

We can format those partitions using mkfs:

	$ mkfs.vfat test/1
	$ mkfs.vfat test/2

Lastly we unmount mbrfs:

	$ fusermount -u test

If you now write `test.img` to an USB drive it'll show up as two FAT partitions.