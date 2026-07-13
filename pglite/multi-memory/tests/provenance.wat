(module
  (import "env" "memory" (memory $memory 2 16 shared))
  (import "env" "__stack_pointer" (global $stack (mut i32)))
  (import "GOT.mem" "private_slot" (global $slot (mut i32)))

  (func $palloc (export "palloc") (param $address i32) (result i32)
    (local.get $address)
  )

  (func $internal (param $address i32) (result i32)
    (i32.load (local.get $address))
  )

  (func (export "constant") (result i32)
    (i32.load (i32.const 96))
  )

  (func (export "stack") (result i32)
    (i32.load
      (i32.sub (global.get $stack) (i32.const 8))
    )
  )

  (func (export "got") (result i32)
    (i32.load (global.get $slot))
  )

  (func (export "allocator_and_internal") (result i32)
    (call $internal
      (call $palloc (i32.const 128))
    )
  )

  (func (export "unknown") (param $address i32) (result i32)
    (i32.load (local.get $address))
  )
)
