project('multilane-server', 'c', default_options: ['c_std=gnu11'])

cc = meson.get_compiler('c')
dpdk_dep = dependency('libdpdk', required: true)
rocksdb_dep = dependency('rocksdb', required: true, method: 'pkg-config')

sources = files(
    'applications/application_shim.c',
    'applications/memcached/interface.c',
    'applications/memcached/protocol.c',
    'applications/memcached/store.c',
    'applications/rocksdb/interface.c',
    'applications/rocksdb/protocol.c',
    'applications/rocksdb/store.c',
    'applications/synthetic/interface.c',
    'applications/synthetic/workload.c',
    'args.c',
    'flow_migration.c',
    'init.c',
    'irq_config.c',
    'network/net_layer2.c',
    'network/net_layer3.c',
    'network/net_layer4.c',
    'network/rx.c',
    'network/tx.c',
    'shared_buffer.c',
    'state.c',
    'syscalls.c',
    'uintr.c',
    'worker.c',
)

required_flags = ['-muintr', '-mno-red-zone']
foreach flag : required_flags
    if not cc.has_argument(flag)
        error('Compiler does not support required flag "@0@" for multilane-server'.format(flag))
    endif
endforeach

executable(
    'multilane-server',
    sources,
    c_args: required_flags,
    dependencies: [dpdk_dep, rocksdb_dep],
    install: true
)
