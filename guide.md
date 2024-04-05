# op guide

## supabase

supabase login
supabase projects list
supabase db push
supabase functions deploy --no-verify-jwt
supabase secrets list
supabase secrets set OPENAI_API_KEY=<your-openai-api-key>


## raspberry



cd devices/raspizerow
scp main.cpp raspberrypi.local:~/raspizerow/

### run

    ```sh
    ssh raspberrypi.local
    cd raspizerow
    sh compile.sh
    gdb main
    ```


#### 连接蓝牙

* 安装依赖

    ```sh
    sudo apt install bluetooth pi-bluetooth bluez blueman
    ```

* 连接蓝牙音箱
  
  ```sh
   bluetoothctl agent on
   bluetoothctl scan on
   bluetoothctl pair [mac addr]
   bluetoothctl trust [mac addr]
   bluetoothctl connect [mac addr]
  ```

* 播放
  
    ```sh
    aplay demo.wav
    ```