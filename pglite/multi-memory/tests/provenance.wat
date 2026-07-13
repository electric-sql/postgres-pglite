(module
  (import "env" "memory" (memory $memory 2 16 shared))
  (import "env" "__stack_pointer" (global $stack (mut i32)))
  (import "GOT.mem" "private_slot" (global $slot (mut i32)))

  (func $palloc (export "palloc") (param $address i32) (result i32)
    (local.get $address)
  )

  (func $private_identity (export "pgl_private_pointer")
    (param $address i32) (result i32)
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

  (func (export "marked") (param $address i32) (result i32)
    (i32.load (call $private_identity (local.get $address)))
  )

  (func $marked_parameter (export "marked_parameter")
    (param $address i32) (result i32)
    (local.set $address
      (call $private_identity (local.get $address))
    )
    (i32.load (local.get $address))
  )

  ;; A conditional marker must not classify the parameter for the whole body.
  (func (export "conditional_marked")
    (param $address i32) (param $mark i32) (result i32)
    (if (local.get $mark)
      (then
        (local.set $address
          (call $private_identity (local.get $address))
        )
      )
    )
    (i32.load (local.get $address))
  )

  (func (export "loop") (param $address i32) (param $count i32) (result i32)
    (local $sum i32)
    (block $done
      (loop $next
        (br_if $done (i32.eqz (local.get $count)))
        (local.set $sum
          (i32.add
            (local.get $sum)
            (i32.load (local.get $address))
          )
        )
        (local.set $address
          (i32.add (local.get $address) (i32.const 4))
        )
        (local.set $count
          (i32.sub (local.get $count) (i32.const 1))
        )
        (br $next)
      )
    )
    (local.get $sum)
  )
)
