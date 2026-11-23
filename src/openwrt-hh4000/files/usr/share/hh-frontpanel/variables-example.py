# Custom variables can be reused in Python-mode drawing commands.
from hhpanel import Panel
import time
with Panel() as panel:
    counter = 0
    while True:
        panel.publish({"counter": counter})
        panel.write("Counter {app.counter}", "{time}").show()
        counter += 1
        time.sleep(1)
