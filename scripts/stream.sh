RTSP_URL=/Users/hnimtadd/Project/Learn/cmake/learn-cpp/assets/sample.mp4
PUBLISHING_URL=rtsp://171.244.62.138:9554/live/demo
REMUX_URL=rtsp://171.244.62.138:9554/live/demo2
$(which ffmpeg) \
	-stream_loop -1 \
	-re \
	-i $RTSP_URL \
	-vcodec libx264 \
	-rtsp_transport tcp \
	-filter:v fps=10 \
	-force_key_frames "expr:gte(t,n_forced*5)" \
	-an -sn -f rtsp \
	$PUBLISHING_URL
