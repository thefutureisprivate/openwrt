#!/usr/bin/env python3

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from concurrent.futures import ThreadPoolExecutor
from ftplib import FTP, all_errors
from socket import timeout as SocketTimeout
from threading import Event
from os import stat


def fit_identity(image, fdtget):
	# Use libfdt's reader instead of interpreting node names as raw strings.
	with tempfile.NamedTemporaryFile() as fit:
		image.seek(72)
		shutil.copyfileobj(image, fit)
		fit.flush()
		try:
			result = subprocess.run(
				[fdtget, '-t', 's', fit.name, '/configurations', 'default'],
				check=True, capture_output=True, text=True, timeout=30)
			name = result.stdout.strip()
			match = re.fullmatch(r'[A-Za-z0-9,_+.-]+_HW([0-9]+)_config_([0-9]+)', name)
			if not match:
				raise ValueError('FIT default configuration has no AVM hardware identity')
			subprocess.run(
				[fdtget, '-p', fit.name, '/configurations/' + name],
				check=True, capture_output=True, timeout=30)
		except FileNotFoundError:
			raise ValueError('FIT validation requires fdtget (device-tree-compiler)')
		except (subprocess.SubprocessError, UnicodeError):
			raise ValueError('cannot read the FIT default configuration')
	return tuple(int(value) for value in match.groups())


def read_environment(ftp):
	complete = Event()

	def receive(connection):
		environment = bytearray()
		while True:
			try:
				data = connection.recv(8192)
			except SocketTimeout:
				if complete.is_set():
					break
				continue
			if not data:
				break
			if len(environment) + len(data) > 1024 * 1024:
				raise ValueError('EVA environment exceeds 1 MiB')
			environment.extend(data)
		return environment

	ftp.voidcmd('TYPE I')
	with ftp.transfercmd('RETR env') as connection, ThreadPoolExecutor(max_workers=1) as reader:
		# EVA may send 226 without closing the data connection. Read both
		# channels concurrently, then drain data until idle after the reply.
		connection.settimeout(1)
		try:
			incoming = reader.submit(receive, connection)
			if ftp.voidresp()[:3] != '226':
				raise ValueError('EVA did not acknowledge the environment transfer')
		finally:
			complete.set()
		environment = incoming.result()
	values = {}
	for line in environment.decode('ascii').splitlines():
		fields = line.split(None, 1)
		if not fields:
			continue
		key, value = fields[0], fields[1].strip() if len(fields) == 2 else ''
		if key in values:
			raise ValueError('duplicate EVA environment key: ' + key)
		values[key] = value
	return environment, values


def save_environment(path, environment):
	# Do not replace an earlier snapshot or expose unit-specific credentials.
	fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
	with os.fdopen(fd, 'wb') as backup:
		backup.write(environment)
		backup.flush()
		os.fsync(backup.fileno())


parser = argparse.ArgumentParser(description='Tool to boot AVM EVA ramdisk images.')
parser.add_argument('ip', type=str, help='IP-address to transfer the image to')
parser.add_argument('image', type=str, help='Location of the ramdisk image')
parser.add_argument('--offset', type=lambda x: int(x,0), help='Offset to load the image to in hex format with leading 0x. Only needed for non-lantiq devices.')
parser.add_argument('--ramsize', type=lambda x: int(x,0), help='RAM size in hex format with leading 0x.')
parser.add_argument('--ramstart', type=lambda x: int(x,0), help='RAM start address in hex format with leading 0x.')
parser.add_argument('--fit', action='store_true', help='Boot an AVM-headered FIT image without requesting a firmware update.')
modes = parser.add_mutually_exclusive_group()
modes.add_argument('--dry-run', action='store_true', help='Print the transfer layout without connecting to EVA.')
modes.add_argument('--check-only', action='store_true', help='Check EVA identity and memory without changing boot settings or uploading.')
parser.add_argument('--save-env', metavar='FILE', help='Save the original EVA environment to a new private file; required for FIT uploads.')
parser.add_argument('--fdtget', default='fdtget', help='Path to fdtget, used to validate FIT configuration identity.')
parser.add_argument('--expected-hwrevision', type=int, help='Require this HWRevision from EVA before uploading.')
parser.add_argument('--expected-hwsubrevision', type=int, help='Require this HWSubRevision from EVA before uploading.')
args = parser.parse_args()

if args.fit and (args.offset is not None or args.ramsize is None or args.ramstart is None):
	parser.error('--fit requires --ramsize and --ramstart and does not support --offset')
if args.fit and not (args.dry_run or args.check_only or args.save_env):
	parser.error('--fit uploads require --save-env')
if args.dry_run and args.save_env:
	parser.error('--dry-run cannot save an EVA environment without connecting')

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
	try:
		with open(args.image, 'rb') as image:
			header = image.read(80)
			if len(header) != 80 or header[:4] != b'\x0d\x00\xed\xfe' or header[72:76] != b'\xd0\x0d\xfe\xed':
				raise ValueError('image does not contain an AVM FIT header')
			fit_size = struct.unpack_from('<I', header, 4)[0]
			if fit_size != struct.unpack_from('>I', header, 76)[0] or fit_size < 40 or fit_size + 72 > size:
				raise ValueError('invalid or truncated FIT image')
			identity = fit_identity(image, args.fdtget)
	except (OSError, ValueError) as err:
		parser.error(str(err))
	for actual, expected in zip(identity, (args.expected_hwrevision, args.expected_hwsubrevision)):
		if expected is not None and actual != expected:
			parser.error('FIT configuration does not match the requested hardware revision')

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

ftp = FTP()

def adam(cmd):
	print("> %s"%(cmd))
	resp = ftp.sendcmd(cmd)
	print("< %s"%(resp))
	if resp[0:3] != "200":
		raise RuntimeError("EVA rejected command: %s" % cmd)

# The following parameters allow booting the avm recovery system with this
# script.
settings_changed = False
uploaded = False
try:
	ftp.connect(args.ip, timeout=30)
	ftp.login('adam2', 'adam2')
	ftp.set_pasv(True)
	if args.fit or args.check_only or args.save_env or args.expected_hwrevision is not None or args.expected_hwsubrevision is not None:
		adam('MEDIA SDRAM')
		environment, values = read_environment(ftp)
		if args.save_env:
			save_environment(args.save_env, environment)
			print('Saved original EVA environment to ' + args.save_env)
		expected_identity = identity if args.fit else (args.expected_hwrevision, args.expected_hwsubrevision)
		for key, expected in zip(('HWRevision', 'HWSubRevision'), expected_identity):
			if expected is not None and values.get(key) != str(expected):
				raise ValueError('EVA %s does not match the requested image/device' % key)
		try:
			reported_size = int(values['memsize'], 0)
			first_free = int(values['firstfreeaddress'].split(',')[0], 0)
		except (KeyError, ValueError):
			raise ValueError('EVA did not report a valid memory size and first free address')
		if ram_size > reported_size:
			raise ValueError('RAM window exceeds the memory reported by EVA')
		if not ram_start <= first_free < start_addr:
			raise ValueError('upload would overlap EVA reserved memory')
	if args.check_only:
		print('EVA preflight passed; no boot settings changed and no image uploaded.')
	else:
		for command in commands:
			if command.startswith('SETENV '):
				settings_changed = True
			adam(command)
		with open(args.image, 'rb') as img:
			ftp.storbinary(store, img)
		uploaded = True
		if args.fit:
			ftp.quit()
except (all_errors + (ValueError, RuntimeError, KeyboardInterrupt)) as err:
	if uploaded:
		print('Upload was acknowledged, but boot could not be confirmed. Check the serial console before retrying.', file=sys.stderr)
	elif settings_changed:
		print('Boot settings may have changed. No QUIT was sent. Check the original memsize and kernel_args_tmp before retrying.', file=sys.stderr)
		if args.save_env:
			print('Original EVA environment: ' + args.save_env, file=sys.stderr)
		# After an interrupted transfer the FTP control stream may still contain
		# pending replies. Do not issue rollback commands or QUIT on that stream.
	parser.exit(1, 'EVA RAM boot failed: %s\n' % (err or 'interrupted'))
finally:
	ftp.close()
