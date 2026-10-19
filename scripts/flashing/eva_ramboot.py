#!/usr/bin/env python3

import argparse
import struct

from ftplib import FTP
from os import stat

parser = argparse.ArgumentParser(description='Tool to boot AVM EVA ramdisk images.')
parser.add_argument('ip', type=str, help='IP-address to transfer the image to')
parser.add_argument('image', type=str, help='Location of the ramdisk image')
parser.add_argument('--offset', type=lambda x: int(x,0), help='Offset to load the image to in hex format with leading 0x. Only needed for non-lantiq devices.')
parser.add_argument('--ramsize', type=lambda x: int(x,0), help='RAM size in hex format with leading 0x.')
parser.add_argument('--ramstart', type=lambda x: int(x,0), help='RAM start address in hex format with leading 0x.')
parser.add_argument('--fit', action='store_true', help='Boot an AVM-headered FIT image without requesting a firmware update.')
parser.add_argument('--dry-run', action='store_true', help='Print the transfer layout without connecting to EVA.')
parser.add_argument('--expected-hwrevision', type=int, help='Require this HWRevision from EVA before uploading.')
parser.add_argument('--expected-hwsubrevision', type=int, help='Require this HWSubRevision from EVA before uploading.')
args = parser.parse_args()

if args.fit and (args.offset is not None or args.ramsize is None or args.ramstart is None):
	parser.error('--fit requires --ramsize and --ramstart and does not support --offset')

ram_size = args.ramsize if args.ramsize is not None else 0x8000000
ram_start = args.ramstart if args.ramstart is not None else 0x80000000

end_addr = ram_start + ram_size

try:
	size = stat(args.image).st_size
except OSError as err:
	parser.error(str(err))
# arbitrary size limit, to prevent the address calculations from overflows etc.
if not 0 < size < ram_size or ram_start < 0 or end_addr > 0xffffffff:
	parser.error('image does not fit in the specified 32-bit RAM window')
if not args.fit and size >= 0x2000000:
	parser.error('legacy RAM image must be smaller than 32 MiB')

if args.fit:
	if (ram_start | ram_size) & 0xffff:
		parser.error('FIT RAM window must be aligned to 64 KiB')
	with open(args.image, 'rb') as image:
		header = image.read(80)
	if len(header) != 80 or header[:4] != b'\x0d\x00\xed\xfe' or header[72:76] != b'\xd0\x0d\xfe\xed':
		parser.error('image does not contain an AVM FIT header')
	fit_size = struct.unpack_from('<I', header, 4)[0]
	if fit_size != struct.unpack_from('>I', header, 76)[0] or fit_size < 40 or fit_size + 72 > size:
		parser.error('invalid or truncated FIT image')

if args.offset is not None:
	memsize = size
	start_addr = args.offset
else:
	# We need to align the address.
	# A page boundary seems to be sufficient on 7362sl and 7412
	# AVM's FIT recovery loader reserves a multiple of 64 KiB.
	alignment = 0x10000 if args.fit else 0x1000
	memsize = (ram_size - size) & -alignment
	start_addr = ram_start + memsize

if start_addr < ram_start or start_addr + size > end_addr or memsize <= 0:
	parser.error('invalid upload address range')

kernel_args = 'mtdram1=0x%08x,0x%08x' % (start_addr, end_addr)
if args.fit:
	kernel_args = '"%s mtdparts_ext=update-image.0:0x%x@0x0(fit-image)"' % (kernel_args, end_addr - start_addr)

commands = [
	'SETENV memsize 0x%08x' % memsize,
	'SETENV kernel_args_tmp ' + kernel_args,
]
if args.fit:
	commands.insert(0, 'MEDIA SDRAM')
else:
	commands.append('MEDIA SDRAM')
store = 'STOR 0x%08x 0x%08x' % (start_addr, end_addr)
if args.dry_run:
	print('\n'.join(commands + [store]))
	raise SystemExit(0)

ftp = FTP(args.ip, 'adam2', 'adam2', timeout=30)

if args.expected_hwrevision is not None or args.expected_hwsubrevision is not None:
	environment = bytearray()
	ftp.sendcmd('MEDIA SDRAM')
	ftp.retrbinary('RETR env', environment.extend)
	values = {}
	for line in environment.decode('ascii', errors='replace').splitlines():
		fields = line.split(None, 1)
		if len(fields) == 2:
			values[fields[0]] = fields[1].strip()
	for key, expected in (('HWRevision', args.expected_hwrevision),
	                      ('HWSubRevision', args.expected_hwsubrevision)):
		if expected is not None and values.get(key) != str(expected):
			ftp.close()
			parser.error('EVA %s does not match the requested device' % key)
	try:
		reported_size = int(values['memsize'], 0)
		first_free = int(values['firstfreeaddress'].split(',')[0], 0)
	except (KeyError, ValueError):
		ftp.close()
		parser.error('EVA did not report a valid memory size and first free address')
	if ram_size > reported_size:
		ftp.close()
		parser.error('RAM window exceeds the memory reported by EVA')
	if not ram_start <= first_free < start_addr:
		ftp.close()
		parser.error('upload would overlap EVA reserved memory')

def adam(cmd):
	print("> %s"%(cmd))
	resp = ftp.sendcmd(cmd)
	print("< %s"%(resp))
	if resp[0:3] != "200":
		raise RuntimeError("EVA rejected command: %s" % cmd)

ftp.set_pasv(True)
# The following parameters allow booting the avm recovery system with this
# script.
try:
	for command in commands:
		adam(command)
	with open(args.image, 'rb') as img:
		ftp.storbinary(store, img)
	if args.fit:
		ftp.quit()
finally:
	ftp.close()
