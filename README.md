Simple proxy example to redirect traffic from one port to another host/port.
The route example can be found in config file. 

Note: This proxy is NOT about performance. 
This is a practical example of callback technique for basic socket select/read/write operations. 

The following configuration example will redirect all traffic on port 8080 from localhost to localhost port 22:

port: 8080<br/>
route: localhost localhost:22<br/>

Test with ssh client:<br/>
ssh -p 8080 localhost
