# A6L tap focus follow-on design — 3 October 2026

This is a bounded design, not part of the frozen controls payload. First prove
rear lens discovery, physical motion and continuous center AF on that payload.
The current HAL honestly advertises no selectable regions. The exact bundled
CameraX1.7.0-alpha03 rejects Aperture's tap metering action when all region counts
are0; advertising AUTO/CONTINUOUS by itself cannot make that action work.

## Minimum implementation

Support one rear AF rectangle only. Keep AE/AWB region counts0 and leave the
front/wide fixed-focus. Advertise maxRegionsAF=1 only after all paths below work.
Do not implement tap exposure, face detection or multi-region weighting here.

1. Translate Android CONTROL_AF_REGIONS (left,top,right,bottom,weight) into real
   libcamera AfWindows. Validate entry count, use signed64-bit arithmetic for
   intersections, and clip to the sensor's active/visible crop. Weight0 and an
   empty region restore the default center focus window. Echo the applied region
   in Android results rather than the original rejected/clipped request.
2. Associate the ROI with its actual request sequence. SoftwareIsp::queueRequest
   already sees the ControlList, and process(frame,input,output) queues a copied
   frame invocation to the ISP worker. Store a bounded per-frame ROI and pass it
   with that invocation; do not mutate a shared rectangle from the Android or
   IPA thread while worker rows are running. Clear queued ROI state at stop.
   A per-frame argument avoids another asynchronous IPA event solely for ROI.
3. Add an independent focus rectangle to SwStatsCpu. Its existing window_ also
   defines global AE/AWB statistics, so changing setWindow() for a tap would
   silently change exposure and white balance. Keep those sums/histograms fixed.
   Replace the center-half row/column sharpness tests with the selected focus
   rectangle, aligned to RAW10 packing and Bayer sample geometry.
4. Accumulate local green mean/sample count alongside local squared differences.
   Existing focus normalization divides by global mean green; a bright or dark
   selected patch needs local normalization so exposure changes do not mimic a
   focus peak. Include ROI identity/generation in statistics so an AF scan cannot
   mix the previous ROI's measurements with a new tap's scan.
5. On accepted ROI change, reset contrast history and settling. In continuous
   mode restart the scan; in AUTO wait for the requested trigger. Preserve
   trigger/cancel/lock semantics and bounded travel. A tiny/textureless valid ROI
   must return a real failure rather than claim focus.
6. Expose AfWindows only with a real focus lens and a RAW10 statistics backend
   supporting ROI. Then enable the single Android AF region and verify CameraX
   AF_TRIGGER_START reaches the IPA. CameraX already handles preview rotation
   and mirroring; do not rotate sensor-coordinate rectangles a second time.

## Coordinate prerequisite found in current source

IMX576 sns_get_selection() currently returns(0,0,5760,4312) for every target and
mode. Actual2880x1620 mode registers give native y-start0x0218=536,
y-end0x0ebf=3775, x-start0, x-end5759, with2x2 binning. Its real analog crop is
(0,536,5760,3240). Full/binned4:3 modes use the full active rectangle.

DebayerCpu::configure() center-crops raw input to the requested output size. For
recorded1600x1200 rear preview, raw2880x1620 has processed window
(640,210,1600,1200), equivalent to native(1280,956,3200,2400). Stats coordinates
are relative to that processed window. Simply scaling a tap against5760x4312
would select the wrong subject, especially near the top/bottom or sides.

Before enabling regions, establish the effective visible crop exposed to
CameraX and map native→mode raw→ISP processed-window coordinates exactly. Correct
per-mode sensor crop reporting is a small sensor-driver proposal for root review.
Also audit Android scaler crop/result semantics: CPU output currently center
crops instead of resizing the whole field of view. Either honor the requested
viewport with real resampling, or expose truthful supported viewport/crop
behavior. Do not pretend the whole sensor maps to the cropped preview.

## Minimum meaningful regression coverage

- Register-table-derived crop checks for each IMX576 mode, including y536 offset
  and2x2 binning. Actual mapper tests cover nonzero active-area origin, overflow,
  off-screen rectangles and Bayer/RAW10 alignment.
- Actual SwStatsCpu memfd RAW10 frames with detailed patches on left/right:
  changing ROI changes sharpness/local mean correctly for all four Bayer orders
  and padded rows. Global AE histogram/AWB sums remain identical.
- Actual AF feedback from selected statistics: left/right selections converge
  to different simulated lens peaks; mid-scan ROI changes discard old measures;
  default reset/cancel work; low texture fails; DAC limits are always respected.
- Several queued requests with different ROIs processed after newer requests
  arrive retain the correct frame ROI. Stop/cancellation clears state, invalid
  requests cannot leave stale windows or unbounded maps.
- Actual HAL parsing gates capability by lens/backend, maps five-int regions
  correctly and returns applied clipped rectangles. Finally perform an attended
  CameraX tap on separated physical targets, recording trigger, ROI generation,
  actual lens movement and resulting sharpness.

This adds one honest AF region. It needs no fake AE/AWB controls and should not
be bundled before current physical AF and crop mapping are understood.
