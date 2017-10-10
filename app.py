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
from configparser import *

class shm_mem:
	def __init__(self, mem, mem_size, mem_lock, dict, layout):
		self.memory = posix_ipc.SharedMemory(mem, posix_ipc.O_CREAT ,size=int(mem_size))
		self.mapmem = mmap.mmap(self.memory.fd, self.memory.size)
		self.memory.close_fd()
		self.s = struct.Struct(layout)
		if mem_lock == "0":
			self.lock = 0
		else:
			self.lock = posix_ipc.Semaphore(mem_lock, posix_ipc.O_CREAT)
		self.keys = [dict.split()]

	def read(self):
		if self.lock != 0:
			self.lock.acquire()
			values = self.s.unpack(self.mapmem[:self.s.size])
			d = dict(zip(self.keys, values))
			self.lock.release()
			return d
		else:
			values = self.s.unpack(self.mapmem[:self.s.size])
			print(self.keys)
			print(values)
			d = dict(zip(self.keys, values))
			return d
			
	def close(self):
		self.mapmem.close()	
		
shm_mems = []
config = ConfigParser()
config.optionxform = str
config_file = open("./config.txt", "r")
config.read_file(config_file)
for section in config.sections():
	s = shm_mem(config.get(section, 'name'), config.get(section, 'size'), config.get(section, 'lock'), config.get(section, 'dict'), config.get(section, 'layout'))
	shm_mems.append(s)



mq = posix_ipc.MessageQueue('/PY_MQ', posix_ipc.O_CREAT, max_message_size=64)
#Any process including C apps can send message on this
def mq_handler(message):
	m = struct.Struct('I') # data structure for message received
	i, =  m.unpack(message[:m.size])
	if i < len(shm_mems):
		mem = shm_mems[i].read()
		print(mem)
	else:
		print("out of list")


#wait for messages on queue
def watch_posix_q():
	global exit
	exit = 0
	while exit == 0:
		try:
			message, priority = mq.receive(0)
			mq_handler(message)
		except:
			time.sleep(0.001) #necessary else this eventlet thread will not allow flask eventlet thread to run
t = threading.Thread(target=watch_posix_q)
t.start()

#open flask app
app = Flask(__name__)
socketio = SocketIO(app)

@app.route("/")
def index():
	return render_template('index.html')

if __name__ == '__main__':
	socketio.run(app, host='0.0.0.0', debug=False)
	
	#stop thread
	exit = 1
	t.join() 
	
	#cleanup channels shm
	for m in shm_mems:
		m.close()

	#cleanup ipc message queue
	mq.close()
