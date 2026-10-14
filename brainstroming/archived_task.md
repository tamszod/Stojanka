Project Strojanka

It is a Discord bot that notifies the user
if a monitored stock has reached the PEAK value or the SUPPORT value of the stock's current cycle,
and tells the user whether they should sell or buy certain stocks.

Adding or removing stocks:
Each Discord user can add or remove the stocks that they wish to have monitored.

Monitoring stocks:
The bot should have a process that checks the stocks, even in parallel, and makes reports. Make sure the reports are efficient.
When a stock is newly recorded, the stock watcher should make a basic report of it based on its historical data. As time goes on and the bot figures out better
variables to use, it should start adjusting to the particular stock's behaviour. It should learn from its mistakes. Our current goal is to be as accurate as possible when deciding
whether a stock has reached its peak or support level. We should also categorize the stocks. What types of stocks are there? Let's discuss it.

Buying, selling:
The user should be able to tell the bot how many shares they bought and for how much. They should also be able to see the profit they made.

Technology:
I was thinking of using a simple single-file SQLite database that can be easily moved if the program migrates.
For the code base I was thinking of C++.
Make it easy: have a build_for_mac.sh and a build_for_windows.bat file that build the whole project and place StrojankaInstaller.exe in the ./dist folder.

Installer:
Our end product should be an installer, with a Mac version and a Windows version.
It should ask where to install, for the Discord bot credentials, whether to create a new SQLite database or use an existing one, etc.
Also make sure that the database can be migrated if it is an old one and new fields or tables have been added.

The bot should have the following commands:

help -
    List the commands and how to use them.

status -
    List of stocks that are being watched.
    trend: increasing, decreasing, stagnating.
    balance: how much money I have in them.

profit -
    The user's profit for each stock (selling price - buying price).

profit <STOCK> -
    The user's profit for <STOCK> (selling price - buying price).

monitor <STOCK> -
    Add the stock to the user's monitoring list.

remove <STOCK> -
    Remove a monitored stock from the user's list.
    If the user still holds shares of it, refuse and tell the user that monitoring cannot be removed while they hold shares.

buy <STOCK> <STOCK_SHARES> <PAID> -
    Record a purchase of shares.
    If the stock is not monitored yet, add it to the monitoring list.

    Example:
    buy FCX 0.858291457 59.78$

sell <STOCK> <STOCK_SHARES> <SOLD_FOR> -
    Sell shares and record the received value.

    Example:
    sell FCX 0.858291457 67.87$

The bot should send the following messages:
    - <STOCK> has reached PEAK. You have <STOCK_SHARES>. It is time to sell. (only if the user has money in it)
    - <STOCK> has reached SUPPORT level. It is time to buy.


In the folder agent_documentation:
create deep documentation that lets AIs read through the application quickly and know where everything is.
In the folder documentation:
create documentation for humans; make it more human-readable.

In the future I have many plans to expand this project, so I hope you make the project scale well.
I also want versioning, so this is our first version.

Before you create the proper documentation, let's talk through this task. When finished, rename the task to archived_task.md. Before that, please fix the grammar mistakes :).
