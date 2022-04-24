# CongestionControlBenchmark
## What's this? 
This project is completely equivalent [DrainQueueCongestion](https://github.com/SoonyangZhang/DrainQueueCongestion), with only a slight optimization. Therefore, please refer to DrainQueueCongestion for the use method.  
当然你也可以顺便参考原项目老哥写的一篇中文说明：https://blog.csdn.net/u010643777/article/details/106761440
## Optimization
1.The experimental scene was modified to three flows. Flow1 and flow2 start at the beginning of the experiment; Flow3 starts at 50 seconds and Flow2 ends at 200s. In order to facilitate user modification, comments are added to the code.  
2.The resulting image output is optimized and the resulting image will be smoother. The resulting images of the original project fluctuates greatly, and even cannot be used because the sawtooth is too dense. **So you need to use *my_3.sh* provided by this project instead of the original *bw_plot.sh* to generate the result images.**
