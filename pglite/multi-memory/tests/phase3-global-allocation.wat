(module
  (memory $memory (import "env" "memory") 1 32768 shared)

  (func (export "store_u32") (param $pointer i32) (param $value i32)
    (i32.store (local.get $pointer) (local.get $value)))

  (func (export "load_u32") (param $pointer i32) (result i32)
    (i32.load (local.get $pointer)))

  (func (export "atomic_add_u32")
    (param $pointer i32) (param $value i32) (result i32)
    (i32.atomic.rmw.add (local.get $pointer) (local.get $value)))

  (func (export "copy_bytes")
    (param $destination i32) (param $source i32) (param $length i32)
    (memory.copy
      (local.get $destination)
      (local.get $source)
      (local.get $length)))
)
