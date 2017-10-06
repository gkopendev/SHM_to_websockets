Need python3 installed.
Will need extra packages - posix_ipc, flask, flask_socketio, eventlet
Easiest way is to pip install the packages (python3)
Compile C app using gcc app.c -lrt -pthread -o app
Run C app first - ./app
Then run python app.py

You should see websockets emitted on browser open at <ip_address:5000>
Initial observations -
flask-socketio takes a lot of CPU.
Fairly high rates of emit socket is possible.
posix ipc python module has some quirks but usable (for e.g. queue notifications
 don't work)

 This is a demo to show python can access C shared memory and message queue. First
 attempt to creating a seamless C shared memory data to web.
 Good e.g to demonstrate using SHM and message queues between C and python.
