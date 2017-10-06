import time
import sched
import mmap
import sys
import posix_ipc
import struct
import datetime
from flask import *
from flask_socketio import SocketIO
import eventlet
eventlet.monkey_patch()
import threading

#same memory C apps can write to
memory = posix_ipc.SharedMemory('/channels_shm', posix_ipc.O_CREAT , size=1024) 
#map that memory to use it
mapmem = mmap.mmap(memory.fd, memory.size)
#close fd as we will use mmap to access memory rather than file descriptor
memory.close_fd()
s = struct.Struct('f f f')	#channels_shm layout

#same memory C apps can write to
locked_memory = posix_ipc.SharedMemory('/eeprom_shm', posix_ipc.O_CREAT , size=4096) 
#map that memory to use it
locked_mapmem = mmap.mmap(locked_memory.fd, locked_memory.size)
#close fd as we will use mmap to access memory rather than file descriptor
locked_memory.close_fd()

semaphore = posix_ipc.Semaphore('/eeprom_sem', posix_ipc.O_CREAT)

# now just use semaphore.acquire() and semaphore.release()
e = struct.Struct('i')	#eeprom_shm layout

#open flask app
app = Flask(__name__)
socketio = SocketIO(app)

mq = posix_ipc.MessageQueue('/PY_MQ', posix_ipc.O_CREAT, max_message_size=64)
#Any process including C apps can send message on this
def mq_handler(message):
	m = struct.Struct('I') # data structure for message received
	i, =  m.unpack(message[:m.size])
	if i == 1 :
		lon, lat, alt = s.unpack(mapmem[:s.size])
		socketio.emit('shm_change', { 'lon': lon, 'lat': lat, 'alt': alt })
		semaphore.acquire()
		eep_test, = e.unpack(locked_mapmem[:e.size])
		semaphore.release()
		socketio.emit('eep_change', {'eep': eep_test})
		print("Ding! %d" % (eep_test))
	else:
		print("no", i)

#wait for messages on queue
def watch_posix_q():
	global exit
	exit = 0
	while exit == 0:
		try:
			message, priority = mq.receive(0)
			mq_handler(message)
		except:
			time.sleep(0.001)
t = threading.Thread(target=watch_posix_q)
t.start()

@app.route("/")
def index():
	return render_template('index.html')

if __name__ == '__main__':
	socketio.run(app, host='0.0.0.0', debug=False)
	
	#stop thread
	exit = 1
	t.join() 
	
	#cleanup channels shm
	mapmem.close()
	
	#cleanup eeprom shm and it's semaphore
	locked_mapmem.close()

	#cleanup ipc message queue
	mq.close()

