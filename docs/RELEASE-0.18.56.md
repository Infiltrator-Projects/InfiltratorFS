# InfiltratorFS 0.18.56

This release carries the Linux native buffered clustered writeback repair introduced after 0.18.55.

Normal Linux writes now enter the page cache instead of forcing the calling userspace thread to synchronously perform compression, integrity and CoW publication work. Dirty folios are written back in bounded contiguous batches while preserving the existing verified CoW data path. The repair also separates visible dirty EOF from persisted EOF, integrates quota reservation with buffered writes, removes the steady-state SHA-256 transform lookup mutex, and bounds IAC1 predictor classification work.

The on-disk format remains 0.18.
