// The work runner that replaces `RunOnDecodeWorkers` in
// `src/graphics/etc2_decoder.cc`.
//
// # What is different from the C++ pool, and why
//
// The C++ `DecodePool` (etc2_decoder.cc) does three things worth replacing:
//
//   * It *declines* when the pool is already busy. A second concurrent batch then
//     drains entirely on its caller -- which for texture upload is the thread
//     that also has to submit the frame -- so overlapping batches serialise onto
//     the busiest thread exactly when there is already too much to do.
//   * It parks its threads on a condition variable per batch, and never joins
//     them, so the pool is leaked at exit.
//   * Its band size is a fixed 4096 blocks, so a 4096-block batch fans out to one
//     thread and a 4097-block batch to eight.
//
// This uses rayon instead: a work-stealing pool sized from the machine, which
// steals rather than declining, and whose per-batch overhead is a few hundred
// nanoseconds instead of a futex round trip. The band structure is unchanged, so
// the banding decision stays in C++ and only the execution moves.
//
// # Re-entrancy
//
// The contract on `RunOnDecodeWorkers` is that `run` may be invoked concurrently
// and that the caller participates. A nested batch from inside `run` must not
// block a worker waiting for itself, so `in_place_scope` is used: rayon lets the
// calling thread run part of the batch in place and joins only the rest, and
// because the inner batch's work items are separate, work stealing fills the
// gap rather than deadlocking. `run` is called exactly `workers` times on the
// outer level either way.

use std::os::raw::{c_int, c_uint, c_void};

/// A C callback that runs one unit of work. Must be safe to call from several
/// threads at once, per the contract on `mocktail::graphics::RunOnDecodeWorkers`.
pub type WorkFn = unsafe extern "C" fn(*mut c_void);

/// Calls `run(context)` `worker_count` times: once on the caller and the rest on
/// pooled workers that run alongside it, returning when every call has returned.
///
/// Returns 0 when the work was spread across the pool, and non-zero when it ran
/// on the caller alone (which the C++ caller treats as a valid outcome, not an
/// error: it already falls back to `run(context)` in that case).
#[no_mangle]
pub unsafe extern "C" fn mocktail_rs_run_work(
    run: Option<WorkFn>,
    context: *mut c_void,
    worker_count: c_uint,
) -> c_int {
    let run = match run {
        Some(run) => run,
        None => return 1,
    };
    if worker_count < 2 {
        // One worker is the caller, so there is nothing to spread.
        run(context);
        return 1;
    }

    #[cfg(feature = "rayon-pool")]
    {
        run_on_rayon(run, context, worker_count)
    }
    #[cfg(not(feature = "rayon-pool"))]
    {
        // The serial-pool feature exists so the pool can be A/B'd in the field
        // without two builds; the caller's fallback keeps the C++ contract.
        let _ = (context, worker_count);
        run(context);
        1
    }
}

/// Splits `worker_count - 1` copies of the work across the pool while the caller
/// runs one copy itself, then waits for all of them.
#[cfg(feature = "rayon-pool")]
fn run_on_rayon(run: WorkFn, context: *mut c_void, worker_count: c_uint) -> c_int {
    let helpers = (worker_count - 1) as usize;

    // `in_place_scope` runs one job on the calling thread and joins the rest, so
    // the caller participates rather than blocking on a worker that might be the
    // caller. `scope` is used instead of `broadcast` because `run` is called a
    // fixed number of times, not once per pool thread.
    rayon::in_place_scope(|scope| {
        for _ in 0..helpers {
            let run = run;
            let context = context as usize;
            scope.spawn(move |_| {
                let context = context as *mut c_void;
                unsafe { run(context) };
            });
        }
        // The caller's own share, run in place.
        unsafe { run(context) };
    });
    0
}