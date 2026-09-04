# Manual — NF Limiter V1.0

**NF AUDIO TOOLS — BY NENNO FERNANDO**

## Fluxo rápido

1. Insira o NF Limiter no último slot do master.
2. Mantenha True Peak ligado e Ceiling em −1,0 dBTP para streaming como ponto inicial.
3. Aumente Gain observando o medidor central de Gain Reduction.
4. Use Clean para transparência, Punch para preservar ataques e Loud para maior densidade.
5. Compare sempre com bypass em volume semelhante.

## Controles

- **Gain:** nível enviado ao limitador.
- **Ceiling:** teto máximo de saída.
- **Release:** velocidade de recuperação da redução.
- **Auto:** adapta o release à intensidade e duração dos picos.
- **Character Clean:** menor coloração.
- **Character Punch:** recuperação mais rápida para preservar impacto.
- **Character Loud:** maior densidade e saturação suave controlada.
- **True Peak:** protege picos reconstruídos entre amostras. A taxa de oversampling usada para essa detecção agora é escolhida automaticamente pela taxa de amostragem do projeto (mais precisão em 44,1/48kHz, proporcionalmente mais leve em taxas altas) — não existe mais um controle de OVERSAMPLING para ajustar, e a latência do plugin nunca muda ao trocar a taxa de amostragem.
- **Delta / Listen:** controle apenas de monitoração, ao lado de CHARACTER. Ativado, permite ouvir exatamente o que o limitador está removendo ou alterando — redução de ganho, coloração do Character e o efeito do Ceiling — em vez da saída normal. Nunca altera o áudio enviado ao host quando desligado, nunca é salvo em preset/sessão, sempre inicia desligado ao carregar uma sessão ou preset, e é cancelado automaticamente enquanto o Bypass está ativo.
- **Stereo Link:** 100% mantém a imagem estéreo estável; valores menores permitem ação parcialmente independente.

## Medidores

Input e Output mostram os canais L/R. Gain Reduction mostra a atenuação aplicada. Peak e True Peak exibem os máximos. LUFS-M indica loudness momentâneo; LUFS-I acumula desde a abertura/reset. CLIP deve permanecer apagado.

## Presets e menu

Escolha um preset na barra superior. Clique SAVE para nomear e salvar. O botão de três traços abre os manuais, pasta de presets, About e Reset Window Size. A alça inferior direita redimensiona a janela. Clique no logo NF para voltar ao tamanho padrão.
