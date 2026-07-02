# SPDX-License-Identifier: BSD-3-Clause

project('multilane-client', 'c', default_options: ['c_std=gnu11'])

cc = meson.get_compiler('c')
dpdk_dep = dependency('libdpdk', required: true)

sources = files(
	'applications/application_shim.c',
	'applications/synthetic.c',
	'applications/memcached/memcached_tx.c',
	'applications/memcached/memcached_rx.c',
	'applications/rocksdb/rocksdb_tx.c',
	'applications/rocksdb/rocksdb_rx.c',
	'args.c',
	'init.c',
	'load_generator.c',
	'network/rx.c',
	'network/tx.c',
	'worker.c',
)

required_flags = ['-muintr', '-mno-red-zone']
foreach flag : required_flags
	if not cc.has_argument(flag)
		error('Compiler does not support required flag "@0@" for multilane-client'.format(flag))
	endif
endforeach

executable(
	'multilane-client',
	sources,
	c_args: required_flags,
	dependencies: [dpdk_dep],
	link_args: ['-lm'],
	install: true,
)
