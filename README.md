# Heltec WiFi LoRa 32 (V3) — Modbus RTU Slave com Sensor AHT20 & MAX485

Este projeto implementa um **Escravo Modbus RTU (Slave)** executado sobre a placa de desenvolvimento **Heltec WiFi LoRa 32 (V3)** (baseada no SoC ESP32-S3). A aplicação realiza leituras periódicas de temperatura e umidade relativa a partir de um sensor **AHT20** via interface **I2C** e disponibiliza as grandezas em registradores do tipo **Holding Registers** acessíveis pela rede industrial **RS-485** via transceptor **MAX485** (com controle automático de fluxo via hardware RTS).

O firmware foi desenvolvido utilizando o framework oficial **ESP-IDF** (FreeRTOS) com a biblioteca de componentes `esp-modbus` (v2.x).

---

## 📌 Principais Características

- **Microcontrolador**: ESP32-S3 dual-core (Heltec WiFi LoRa 32 V3).
- **Sensor Climático**: AHT20 (I2C, endereço padrão `0x38`).
- **Comunicação Serial Industrial**: Modbus RTU Slave sobre RS-485 em modo Half-Duplex.
- **Transceptor RS-485**: Módulo MAX485 operando com lógica e alimentação de 3,3 V.
- **Controle de Fluxo RS-485**: Pino de hardware RTS (`MB_RTS_PIN = 45`) associado nativamente ao driver UART em modo RS-485.
- **Arquitetura FreeRTOS**:
  - `aht20_task`: Amostragem a cada ~500 ms com fila circular de leituras (`QueueHandle_t`).
  - `modbus_event_task`: Desacoplamento de leitura/resposta com monitoramento de requisições do mestre (`MB_EVENT_HOLDING_REG_RD`).

---

## 🛠️ Pinagem e Conexões de Hardware

### 1. Sensor AHT20 (Barramento I2C)
O AHT20 deve ser alimentado em 3.3V com barramento I2C configurado com resistores de pull-up (ativos internamente no ESP-IDF):

| Pino AHT20 | Pino Heltec V3 (ESP32-S3) | Função |
| :--- | :--- | :--- |
| **VCC** | `3V3` | Alimentação (3.3V) |
| **GND** | `GND` | Referência comum |
| **SDA** | `GPIO 47` | I2C Data (`I2C_SDA_PIN`) |
| **SCL** | `GPIO 48` | I2C Clock (`I2C_SCL_PIN`) |

---

### 2. Módulo Transceptor MAX485 (Barramento RS-485)
> **Atenção:** Certifique-se de alimentar o módulo em **3.3V** para manter compatibilidade direta de níveis lógicos com o ESP32-S3 (evitando sobretensão nos pinos de entrada). Caso utilize um MAX485 clássico de 5V ou variantes (como MAX3485 nativo de 3V3), garanta as referências e divisores de tensão adequados.

| Pino Módulo MAX485 | Pino Heltec V3 (ESP32-S3) | Observação |
| :--- | :--- | :--- |
| **VCC** | `3V3` | Alimentação lógica 3.3V |
| **GND** | `GND` | Referência comum |
| **DI** (Driver In) | `GPIO 41` (`MB_TX_PIN`) | Saída TX UART_NUM_1 |
| **RO** (Receiver Out)| `GPIO 42` (`MB_RX_PIN`) | Entrada RX UART_NUM_1 |
| **DE** (Driver Enable)| `GPIO 45` (`MB_RTS_PIN`)| Chaveamento de transmissão |
| **RE** (Receiver Enable)| `GPIO 45` (`MB_RTS_PIN`)| Ligado em conjunto com **DE** |
| **A** (Não inversor)| Barramento RS-485 (Linha A / +) | Par trançado diferencial |
| **B** (Inversor) | Barramento RS-485 (Linha B / -) | Par trançado diferencial |

*Dica de instalação:* Em redes industriais longas, lembre-se de instalar o resistor de terminação de **120 Ω** entre as linhas **A** e **B** nas duas extremidades do barramento.

---

## 📊 Mapeamento Modbus RTU

### Parâmetros de Comunicação Serial
- **Protocolo**: Modbus RTU (Serial RS-485 Half-Duplex)
- **Endereço do Escravo (Slave ID)**: `1`
- **Baud Rate**: `9600 bps`
- **Bits de Dados**: `8`
- **Paridade**: `Nenhuma` (None)
- **Stop Bits**: `1`

### Tabela de Registradores (Holding Registers — Função 0x03)

Os valores são exportados como inteiros com sinal de 16 bits (`int16_t`) com fator de multiplicação **10x** para preservar uma casa decimal sem necessidade de aritmética em ponto flutuante de 32 bits no mestre.

| Endereço do Registrador | Offset | Tipo de Dado | Nome | Unidade | Fator de Escala | Exemplo Lido | Valor Real |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **40001** (ou `0x0000`) | 0 | `INT16` | Temperatura | °C | ÷ 10.0 | `274` | **27.4 °C** |
| **40002** (ou `0x0001`) | 1 | `INT16` | Umidade Relativa | % | ÷ 10.0 | `625` | **62.5 %** |

---

## 💻 Estrutura do Projeto & Dependências

A aplicação utiliza o componente oficial **`espressif/esp-modbus`** (versão 2.x). O arquivo `idf_component.yml` (ou o `main/CMakeLists.txt`) deve garantir a presença do componente.

### Exemplo de `main/idf_component.yml`:
```yaml
dependencies:
  espressif/esp-modbus: "^2"
```

### Arquitetura de Software:
1. **`app_main`**:
   - Cria uma fila (`sensor_queue`) de tamanho 10 para desacoplar as tarefas.
   - Inicializa a interface I2C mestre a 100 kHz.
   - Inicializa o subsistema Modbus com driver RS-485 configurado nativamente com controle de pino RTS.
   - Instancia as tasks `aht20_task` (prioridade 5) e `modbus_event_task` (prioridade 6).
2. **`aht20_task`**:
   - Dispara a conversão de temperatura/umidade via comando `0xAC 0x33 0x00`.
   - Lê os 6 bytes de dados brutos e realiza a conversão matemática linear oficial do sensor.
   - Enfileira a medição escalonada (`int16_t`).
3. **`modbus_event_task`**:
   - Aguarda eventos de leitura do mestre (`mbc_slave_check_event(..., MB_EVENT_HOLDING_REG_RD)`).
   - Atualiza a área de registradores mapeada (`holding_reg_area`) com os dados mais recentes disponíveis.

---

## 🚀 Como Compilar e Gravar

### Pré-requisitos
- **ESP-IDF v5.0+** instalado e configurado nas variáveis de ambiente (`get_idf`).
- Placa **Heltec WiFi LoRa 32 V3** conectada à porta USB.

### Passos:

1. **Definir o target para o ESP32-S3**:
   ```bash
   idf.py set-target esp32s3
   ```

2. **Compilar a aplicação**:
   ```bash
   idf.py build
   ```

3. **Gravar o firmware e abrir o monitor serial**:
   Substitua `/dev/ttyUSB0` (ou `COMx` no Windows) pela porta correspondente:
   ```bash
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

---

## 🧪 Como Testar e Validar

Você pode validar a leitura Modbus utilizando conversores USB-RS485 comuns e ferramentas como **QModMaster**, **Modbus Poll**, ou scripts em Python com **pymodbus** / **minimalmodbus**.

### Teste Rápido via Python (`minimalmodbus`):

```bash
pip install minimalmodbus pyserial
```

```python
import minimalmodbus
import serial

# Configura o instrumento
instrument = minimalmodbus.Instrument('/dev/ttyUSB1', slaveaddress=1) # Ajuste a porta COM/tty
instrument.serial.baudrate = 9600
instrument.serial.bytesize = 8
instrument.serial.parity = serial.PARITY_NONE
instrument.serial.stopbits = 1
instrument.serial.timeout = 0.5
instrument.mode = minimalmodbus.MODE_RTU

# Leitura dos 2 registradores a partir do endereço 0 (função 0x03)
# Dividimos por 10.0 para recuperar o ponto flutuante
temp_raw, hum_raw = instrument.read_registers(registeraddress=0, number_of_registers=2, functioncode=3)

temperatura = temp_raw / 10.0
umidade = hum_raw / 10.0

print(f"Temperatura: {temperatura:.1f} °C")
print(f"Umidade Relativa: {umidade:.1f} %")
```

---

## 📝 Licença
Este projeto é distribuído sob licença MIT. Sinta-se à vontade para utilizar, modificar e adaptar para suas aplicações industriais ou acadêmicas.