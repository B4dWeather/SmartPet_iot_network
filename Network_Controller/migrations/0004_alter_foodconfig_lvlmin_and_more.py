# Generated by Django 4.2.2 on 2023-09-13 16:03

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ('Network_Controller', '0003_alter_liveclient_nodetype'),
    ]

    operations = [
        migrations.AlterField(
            model_name='foodconfig',
            name='lvlMin',
            field=models.IntegerField(default=0),
        ),
        migrations.AlterField(
            model_name='liveclient',
            name='nodeCoapAddress',
            field=models.CharField(blank=True, default='', max_length=30),
        ),
        migrations.AlterField(
            model_name='liveclient',
            name='nodeCoapName',
            field=models.CharField(blank=True, default='', max_length=16),
        ),
    ]
