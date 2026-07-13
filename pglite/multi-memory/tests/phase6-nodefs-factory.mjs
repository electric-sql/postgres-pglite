export default function createPhase6NodeFilesystem({ root, mounts = [] }) {
  if (typeof root !== 'string' || root.length === 0) {
    throw new TypeError('Phase 6 NODEFS factory requires a PGDATA root')
  }
  if (!Array.isArray(mounts)) {
    throw new TypeError('Phase 6 NODEFS mounts must be an array')
  }

  let pg
  return {
    async init(instance, options) {
      pg = instance
      return {
        emscriptenOpts: {
          ...options,
          preRun: [
            ...(options.preRun ?? []),
            (module) => {
              module.FS.mkdirTree('/pglite/data')
              module.FS.mount(
                module.FS.filesystems.NODEFS,
                { root },
                '/pglite/data',
              )
              for (const mount of mounts) {
                if (
                  typeof mount?.root !== 'string' ||
                  typeof mount?.path !== 'string' ||
                  !mount.path.startsWith('/')
                ) {
                  throw new TypeError('invalid Phase 6 NODEFS mount')
                }
                module.FS.mkdirTree(mount.path)
                module.FS.mount(
                  module.FS.filesystems.NODEFS,
                  { root: mount.root },
                  mount.path,
                )
              }
            },
          ],
        },
      }
    },
    async syncToFs() {},
    async initialSyncFs() {},
    async dumpTar() {
      throw new Error('Phase 6 test filesystem does not implement dumpTar')
    },
    async closeFs() {
      pg.Module.FS.quit()
    },
  }
}
