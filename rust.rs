// Rust tour for C++ developers: a tiny task board.
//
// Run with: rustc rust.rs && ./rust
//
// Concept map (C++ -> Rust):
//   1. const-by-default              -> `let` vs `let mut`
//   2. std::variant + visit          -> enum with data + exhaustive `match`
//   3. exceptions / null             -> Result<T, E> / Option<T>, `?` operator
//   4. move semantics (opt-in)       -> moves are the DEFAULT; `.clone()` is explicit
//   5. references / dangling refs    -> borrows `&T` / `&mut T`, checked at compile time
//   6. inheritance / concepts        -> traits, generics (static) and `dyn` (dynamic)
//   7. RAII / destructors            -> `Drop`
//   8. data races                    -> `Send`/`Sync`, `Arc<Mutex<T>>`, rejected at compile time
//   9. ranges / lambdas              -> iterator chains + closures

use std::collections::HashMap;
use std::error::Error;
use std::fmt;
use std::num::ParseIntError;
use std::sync::{Arc, Mutex};
use std::thread;

// ---------------------------------------------------------------------------
// 2. Enums carry data. Think `std::variant`, but the compiler forces `match`
//    to handle every variant. There is no "forgot a case" bug.
//
//    `derive` generates trait implementations (like defaulted special members
//    or operator== in C++): Debug = printing with {:?}, Clone = explicit deep
//    copy, PartialEq = operator==.
// ---------------------------------------------------------------------------
#[derive(Debug, Clone, PartialEq)]
enum Status {
    Todo,
    InProgress { assignee: String },
    Done,
}

// ---------------------------------------------------------------------------
// 3. Errors are ordinary values. There are no exceptions; a function that can
//    fail says so in its return type, and callers cannot ignore it silently.
// ---------------------------------------------------------------------------
#[derive(Debug)]
enum TaskError {
    NotFound(u32),
    AlreadyDone(u32),
    InvalidId(ParseIntError),
}

// `impl Trait for Type` implements an interface for a type after the fact.
// `Display` is what `{}` uses, like operator<< in C++.
impl fmt::Display for TaskError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            TaskError::NotFound(id) => write!(f, "task {id} not found"),
            TaskError::AlreadyDone(id) => write!(f, "task {id} is already done"),
            TaskError::InvalidId(e) => write!(f, "invalid id: {e}"),
        }
    }
}

impl Error for TaskError {}

// `From` lets `?` convert errors automatically (like an implicit converting
// constructor, but only used where an error is propagated).
impl From<ParseIntError> for TaskError {
    fn from(e: ParseIntError) -> Self {
        TaskError::InvalidId(e)
    }
}

// ---------------------------------------------------------------------------
// Structs: fields are private to the module by default. Data and behavior are
// separate: fields in `struct`, methods in `impl`. There is no inheritance.
// ---------------------------------------------------------------------------
#[derive(Debug)]
struct Task {
    id: u32,
    title: String, // owned, heap-allocated text (like std::string)
    status: Status,
}

impl Task {
    // No `self` parameter: an associated function, i.e. a static method.
    // `impl Into<String>` accepts &str or String; the Task then owns its text.
    fn new(id: u32, title: impl Into<String>) -> Self {
        Self {
            id,
            title: title.into(),
            status: Status::Todo,
        }
    }

    // `&mut self`: exclusive, mutable borrow of the object, like a non-const
    // `this`. `&self` would be a const method. The caller needs a `mut` binding.
    fn start(&mut self, assignee: &str) -> Result<(), TaskError> {
        self.ensure_open()?;
        self.status = Status::InProgress {
            assignee: assignee.to_string(), // &str is a view; to_string() copies
        };
        Ok(()) // no `return` needed: the last expression (no `;`) is the value
    }

    fn complete(&mut self) -> Result<(), TaskError> {
        self.ensure_open()?;
        self.status = Status::Done;
        Ok(())
    }

    fn ensure_open(&self) -> Result<(), TaskError> {
        if self.status == Status::Done {
            Err(TaskError::AlreadyDone(self.id))
        } else {
            Ok(())
        }
    }
}

// ---------------------------------------------------------------------------
// 6. Traits are Rust's interfaces/concepts. They can have default methods.
// ---------------------------------------------------------------------------
trait Describe {
    fn describe(&self) -> String;

    fn shout(&self) -> String {
        self.describe().to_uppercase()
    }
}

impl Describe for Status {
    fn describe(&self) -> String {
        // `match` is an expression and must be exhaustive.
        match self {
            Status::Todo => "todo".to_string(),
            Status::InProgress { assignee } => format!("in progress ({assignee})"),
            Status::Done => "done".to_string(),
        }
    }
}

impl Describe for Task {
    fn describe(&self) -> String {
        format!("#{} {} [{}]", self.id, self.title, self.status.describe())
    }
}

// Generic with a trait bound: like a C++ template with a concept. It is
// monomorphized per type, so there is no virtual call. `&[T]` is a slice, i.e.
// a non-owning (pointer, length) view like std::span.
fn print_all<T: Describe>(items: &[T]) {
    for item in items {
        println!("  static : {}", item.describe());
    }
}

// Dynamic dispatch: `dyn Describe` is like a virtual base class. `Box` owns the
// heap object, like std::unique_ptr, so unrelated types can share one Vec.
fn print_dyn(items: &[Box<dyn Describe>]) {
    for item in items {
        println!("  dynamic: {}", item.shout());
    }
}

// ---------------------------------------------------------------------------
// 5. Lifetimes. The returned `&str` points into one of the arguments. `'a`
//    tells the compiler "the result lives no longer than BOTH inputs", so a
//    dangling reference is a compile error instead of undefined behavior.
// ---------------------------------------------------------------------------
fn longer_title<'a>(a: &'a Task, b: &'a Task) -> &'a str {
    if a.title.len() >= b.title.len() {
        &a.title
    } else {
        &b.title
    }
}

// ---------------------------------------------------------------------------
// A container that owns its tasks (HashMap ~ std::unordered_map).
// ---------------------------------------------------------------------------
struct Board {
    tasks: HashMap<u32, Task>,
    next_id: u32,
}

impl Board {
    fn new() -> Self {
        Self {
            tasks: HashMap::new(),
            next_id: 1,
        }
    }

    fn add(&mut self, title: impl Into<String>) -> u32 {
        let id = self.next_id;
        self.next_id += 1;
        self.tasks.insert(id, Task::new(id, title));
        id
    }

    // 3. `Option<&Task>` instead of a nullable pointer: the "absent" case must
    //    be handled before the Task can be touched. The returned reference
    //    borrows from `self`, so the board cannot be modified while it lives.
    fn get(&self, id: u32) -> Option<&Task> {
        self.tasks.get(&id)
    }

    // `ok_or` turns an Option into a Result.
    fn get_mut(&mut self, id: u32) -> Result<&mut Task, TaskError> {
        self.tasks.get_mut(&id).ok_or(TaskError::NotFound(id))
    }

    // 9. Iterator chain: lazy, compiled down to a loop, no temporary vectors.
    //    `|t| ...` is a lambda. The result borrows the titles from `self`.
    fn open_titles(&self) -> Vec<&str> {
        let mut titles: Vec<&str> = self
            .tasks
            .values()
            .filter(|t| t.status != Status::Done)
            .map(|t| t.title.as_str())
            .collect();
        titles.sort(); // HashMap order is unspecified
        titles
    }
}

// `?` on a ParseIntError converts it into TaskError via the From impl above.
fn parse_id(text: &str) -> Result<u32, TaskError> {
    Ok(text.trim().parse::<u32>()?)
}

// ---------------------------------------------------------------------------
// 7. RAII: `Drop` is the destructor. It runs deterministically at end of
//    scope, in reverse order of declaration, exactly like in C++.
// ---------------------------------------------------------------------------
struct Connection(&'static str);

impl Drop for Connection {
    fn drop(&mut self) {
        println!("  closing {}", self.0);
    }
}

// `main` may return a Result; an Err is printed and gives a non-zero exit code.
// `Box<dyn Error>` can hold any error type (here: TaskError).
fn main() -> Result<(), Box<dyn Error>> {
    // 1. Immutable by default. `let mut` is needed to mutate.
    let mut board = Board::new();
    let first = board.add("Learn ownership");
    board.add("Learn traits");
    let third = board.add(String::from("Write real code"));

    println!("== errors as values ==");
    let id = parse_id(" 2 ")?; // `?`: on Err, return early from main
    board.get_mut(id)?.start("alice")?;
    board.get_mut(first)?.complete()?;

    // Handling an error explicitly instead of propagating it.
    match board.get_mut(first)?.complete() {
        Ok(()) => println!("  unexpected success"),
        Err(e) => println!("  expected error: {e}"),
    }
    if let Err(e) = parse_id("abc") {
        println!("  expected error: {e}");
    }

    println!("== Option instead of null ==");
    match board.get(99) {
        Some(task) => println!("  found {}", task.title),
        None => println!("  no task 99"),
    }
    // `if let` handles one pattern; `unwrap()` would panic (abort) on None.
    if let Some(task) = board.get(third) {
        println!("  found {}", task.describe());
    }

    println!("== ownership and moves ==");
    let title = String::from("Moved title");
    let task = Task::new(100, title); // `title` is MOVED into the Task
    // println!("{title}");           // error[E0382]: borrow of moved value
    let copy = task.title.clone(); //  copies are always explicit
    println!("  original: {}, copy: {copy}", task.title);

    // Plain-old-data types (integers, bool, ...) are `Copy`: no move happens.
    let n = 5;
    let m = n;
    println!("  n = {n}, m = {m}");

    println!("== borrowing ==");
    // Rule: any number of `&T` XOR exactly one `&mut T`, at any time.
    // This removes iterator invalidation and aliasing bugs at compile time.
    let a = Task::new(101, "short");
    let b = Task::new(102, "a much longer title");
    let longer = longer_title(&a, &b); // borrows a and b
    println!("  longer title: {longer}");
    // drop(a);                        // error[E0505]: `a` is still borrowed...
    // println!("{longer}");           // ...because `longer` is used afterwards.

    let before = board.get(first).map(|t| t.title.as_str()); // shared borrow
    println!("  before: {before:?}");
    board.add("Added later"); // OK: `before` is not used after this point
    // println!("{before:?}"); // would be error[E0502]: add() needs &mut board

    println!("== iterators and closures ==");
    let min_len = 12;
    let long_open = board
        .open_titles()
        .into_iter()
        .filter(|t| t.len() >= min_len) // closure captures `min_len` by reference
        .count();
    println!("  open: {:?}", board.open_titles());
    println!("  open with >= {min_len} chars: {long_open}");

    println!("== traits ==");
    print_all(&[Task::new(1, "A"), Task::new(2, "B")]); // static dispatch
    let mixed: Vec<Box<dyn Describe>> = vec![
        Box::new(Task::new(3, "C")),
        Box::new(Status::InProgress {
            assignee: "bob".to_string(),
        }),
    ];
    print_dyn(&mixed); // dynamic dispatch

    println!("== RAII ==");
    {
        let _db = Connection("database");
        let _cache = Connection("cache");
        println!("  working...");
    } // dropped here in reverse order: cache, then database

    println!("== threads ==");
    // `Rc` (non-atomic shared pointer) is not `Send`, so using it across threads
    // is a compile error. `Arc` is the atomic shared_ptr; `Mutex<T>` OWNS its
    // data, so the counter cannot be touched without holding the lock.
    let counter = Arc::new(Mutex::new(0u32));
    let handles: Vec<_> = (0..4)
        .map(|_| {
            let counter = Arc::clone(&counter);
            // `move` transfers the clone into the thread's closure.
            thread::spawn(move || {
                *counter.lock().unwrap() += 1; // lock released at end of statement
            })
        })
        .collect();
    for handle in handles {
        handle.join().unwrap();
    }
    println!("  counter = {}", *counter.lock().unwrap());

    Ok(())
}
