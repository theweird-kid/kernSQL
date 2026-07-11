- The internal Storage Engine maps **Record IDs -> physical location of a Tuple**
- But we also need indexes for the Application user like the primary key. My goal is to enforce a primary Key per table. ==**My Intuition**: This indexing will be stored somewhere above the low level storage engine (like a Database catalog) this will have an index file per Table and the index holds a map *Primary Key -> low-level Record ID*==  
- **For now:** I don't plan on creating additional indexes besides the primary key at the application level. But however if we plan to do it, this could be served in the similar manner as the primary_key index, we'll instead just have the map of *specific indexing_column -> low-level Record ID*
## Pages
- Slotted pages to support VARCHAR, need to also put a cap on the max length of VARCHAR. ==I would also need to handle the overflow for tuples.==

## Data Types
- Integers, Floating points, Fixed precision (Decimals), and VARCHAR, NULL (bitmap )
- Fixed precision are stored as char array with metadata associated on how to perceive the value (sign, numbers after decimal, etc)

## Locks Vs Latches
- Locks are high level concept, these are held for a transaction over DB, tables, or rows.
- Latches are low-level and apply to pages

## Page Table and Buffer Pool
- Buffer Pool is the actual memory space that holds the frames, frames are basically the pages from disk read into memory (i.e the Buffer Pool)
- Page Table is a Map of *Page No -> memory address in Buffer Pool*

## Page Directory
- This is on disk mapping of page ID -> page location on the Database files. whereas the Page Table is an in-memory concept
