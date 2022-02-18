ScaleXFS: Getting scalability of XFS back on the ring
-----
* Maintainer : Dohyun Kim(ehgus421210)
* Contributor : Dohyun Kim, Kwangwon Min, Joontaek Oh, Youjip Won

ScaleXFS is the newly developed filesystem which addresses two primary causes for XFS scalability failures: Contention between in-memory logging and on-disk logging and Contention among the multiple concurrent in-memory loggings.
 
To mitigate or eliminate the contention, we have proposed the three key techniques: (i) double committed item list, (ii) per-core in-memory logging, (iii) strided space counting.

ScaleXFS is implemented based on XFS with minimal modification. Since the filesystem on-disk layout remains unchanged, it can mount the existing XFS partition.


Publication
-----
* Dohyun Kim, Kwangwon Min, Joontaek Oh, and Youjip Won "ScaleXFS: Getting scalability of XFS back on the ring", in Proc. of USENIX Conference on File and Storage Technologies (FAST) 2022, Feb, 22-24, 2022 

How to use ScaleXFS
-----

Mount ScaleXFS (same with xfs)
        
        mount -t xfs /dev/sdb /mnt

Mount Options for ScaleXFS

        # Key Techniques
        # (i) Double commited item list
        # (ii) Per-core in-memory logging
        # (iii) Strided space counting (we recommand 8 Kbyte stride length).

        [No option]: XFS + (i)
        -o lspercpu=0: XFS + (i) + (ii)
        -o lspercpu=[stride length]: XFS + (i) + (ii) + (iii)
        ex) -o lspercpu=8k: XFS + (i) + (ii) + (iii) with 8 Kbyte stride length
