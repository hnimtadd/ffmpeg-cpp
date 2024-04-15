RTSP_URL=/Users/hnimtadd/Project/Learn/cmake/learn-cpp/assets/encode_mp4.mp4
PUBLISHING_URL=rtsp://171.244.62.138:9554/live/demo
REMUX_URL=rtsp://171.244.62.138:9554/live/demo2
$(which ffmpeg) \
	-re \
	-stream_loop -1 \
	-i $RTSP_URL \
	-threads 10 \
	-force_key_frames "expr:gte(t,n_forced*5)" \
	-rtsp_transport tcp \
	-filter:v fps=10 \
	-an \
	-sn \
	-f rtsp \
	$PUBLISHING_URL &
(sleep 3 && $(which ffplay) -i "$PUBLISHING_URL") &
wait

# replay the stream
