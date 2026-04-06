> [!IMPORTANT]
> This is a **fork** of the original [Butterscotch](https://github.com/MrPowerGamerBR/Butterscotch) project by [@MrPowerGamerBR](https://github.com/MrPowerGamerBR).
>

## Сборка для Miyoo через Docker

### Предварительные требования

- Установленный Docker (Ubuntu/Debian: `sudo apt install docker.io`)
- Скачанные образы тулчейнов:

```bash
sudo docker pull miyoocfw/toolchain-shared-uclibc
sudo docker pull miyoocfw/toolchain-shared-musl
```

### Локальная сборка

Используйте готовые скрипты:

```bash
# Сборка с uclibc тулчейном
./make_miyoo_uclibc.sh

# Сборка с musl тулчейном
./make_miyoo_musl.sh
```

Скомпилированные файлы появятся в `build_miyoo/`.

### Сборка через Dockerfiles

```bash
# uclibc
docker build -f Dockerfile.miyoo-uclibc -t butterscotch-miyoo-uclibc .
docker run --rm -v "$(pwd)":/src butterscotch-miyoo-uclibc && docker run --rm -v "$(pwd)":/src butterscotch-miyoo-uclibc cmake --build build_miyoo -j$(nproc)

# musl
docker build -f Dockerfile.miyoo-musl -t butterscotch-miyoo-musl .
docker run --rm -v "$(pwd)":/src butterscotch-miyoo-musl && docker run --rm -v "$(pwd)":/src butterscotch-miyoo-musl cmake --build build_miyoo -j$(nproc)
```

### GitHub Actions

Сборка для Miyoo автоматически запускается в CI при каждом push/PR. Артефакты скачивайте со страницы [Actions](../../actions). 
