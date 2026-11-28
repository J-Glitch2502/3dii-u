from analyze_frame_timing import analyze
text='''
 100 [pace] 6 frames in 2000 ms; max gap 180000 us; gaps <=20/35/50/67/100/>100 ms: 1 1 1 1 1 1
 101 [glsl] vertex aabb compiled in 900 ms
 102 [glsl] pixel ccdd compiled in 10 ms, from the cache
 200 [pace] 6 frames in 2100 ms; max gap 160000 us; gaps <=20/35/50/67/100/>100 ms: 1 1 1 1 1 1
 201 [fallback-route] missing vertex 4; forced CPU 0; missing geometry 1; GPU resource limit 0
 202 [pace] 99 frames in 0 ms; max gap 0 us; gaps <=20/35/50/67/100/>100 ms: 1 1 1 1 1 1
'''
a=analyze(text);assert a['frames']==12 and a['measured_ms']==4100
assert a['gaps_over_50ms']==6 and a['gaps_over_100ms']==2
assert a['cold_compiles']['vertex']['count']==1 and a['cold_compiles']['pixel']['count']==0
assert a['cpu_fallback']['missing_vertex']==4 and a['rejected_or_boundary_windows']==1
b=analyze(text,150,201);assert b['frames']==0 # interval crossing scene start is excluded
assert analyze(text,100,201)['frames']==6
assert analyze('')['presentations_per_second'] is None
assert analyze('')['cpu_fallback'] is None
print('PASS: weighted frame windows, hitch buckets, cached compilation exclusion, bounded scene comparisons and malformed report rejection')
